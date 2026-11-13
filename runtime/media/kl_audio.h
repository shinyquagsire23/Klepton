// The CoreAudio output sink — where the guest's PCM finally becomes a sound.
//
// kl_opensl.c serves FMOD an OpenSL ES buffer-queue player. Everything about
// that surface is the guest's contract; everything in here is the host's, and
// the two meet at exactly one call: the feeder pops a buffer and hands it to
// kl_audio_write(), which blocks for as long as that audio takes to play. That
// blocking IS the pacing — before this file existed the feeder timed buffers
// with usleep() and dropped them, which kept FMOD's mixer running and made no
// sound (3,238 buffers consumed, zero heard).
//
// The split matters for one reason: CoreAudio's render callback is real-time
// and must not block, allocate, or take a lock the guest holds. So the guest
// callback stays on the feeder thread, exactly where it already was, and the
// render callback only ever memcpy's out of a lock-free ring this file fills.
// Format conversion and resampling happen on the producer side for the same
// reason — the render thread does no arithmetic it can avoid.
//
// One platform, two subtypes: DefaultOutput on macOS, RemoteIO on
// visionOS/iOS. The session (visionOS only) is Swift's job — see
// kl_audio_session_ready() and KleptonApp.swift.
#ifndef KL_AUDIO_H
#define KL_AUDIO_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// ---- the ONE description of the audio device this runtime presents ----
//
// A guest learns the device's rate and buffer size twice: through Java
// (`AudioManager.getProperty(OUTPUT_SAMPLE_RATE / OUTPUT_FRAMES_PER_BUFFER)`,
// answered in runtime/jni) and again through whichever native sink it opens.
// Those are the same device, so they must be the same numbers, and they were
// not: Java said 256 frames and kl_aaudio.c handed out bursts of 240.
//
// 240 is not a wrong number in isolation — it is 5 ms at 48 kHz, and the
// AAudio ABI lets a HAL pick any burst it likes. It is wrong because of what a
// mixer does with it. Wwise renders in fixed 1024-frame blocks and its sink
// copies out of the block it has; asked for 240 frames five times it can serve
// 240+240+240+240 and then only 64, and it fills the remaining 176 frames of
// our buffer with NOTHING. The feeder zeroes the buffer before every pull, so
// those 176 frames go to the speaker as silence. Repeat every 1200 frames and
// the result is not a dropout — it is a 40 Hz gate on the music, and it also
// costs playing time: 176 frames in every 1200 are ours, so the guest's audio
// runs about 15% behind the wall clock for as long as it plays. A guest-side
// PCM capture of Tetris Effect shows exactly that: 796 of 4000 buffers filled
// to exactly 64 frames, every fifth buffer, for the whole run.
//
// So the burst is a POWER OF TWO, and the same one Java is told. 256 divides
// 1024, 2048, 512 and every other block size a mixer actually uses, which is
// the property that matters; being 5.33 ms rather than 5.00 ms is not.
#define KL_AUDIO_NATIVE_RATE   48000
#define KL_AUDIO_NATIVE_FRAMES 256

// The guest's sample format. Not every guest is FMOD: Wwise on Meta's EPS
// spatialiser renders float32 and opens OpenSL with SLAndroidDataFormat_PCM_EX.
// A float buffer decoded as int16 is not merely distorted — each 4-byte float
// becomes TWO int16 samples, so the stream plays at half speed and the mantissa
// half lands in one channel (white noise at full scale) while the exponent half
// lands in the other (a crude rendering of the signal). That is exactly what
// Batman: Arkham Shadow sounded like before this existed.
typedef enum {
    KL_AUDIO_S16 = 0,   // signed 16-bit, native endian — FMOD's, and the default
    KL_AUDIO_U8,        // unsigned 8-bit, 0x80 = silence
    KL_AUDIO_S24,       // signed 24-bit packed into 3 bytes
    KL_AUDIO_S32,       // signed 32-bit
    KL_AUDIO_F32,       // float32, nominal [-1, 1] (may exceed it; we clamp)
} kl_audio_fmt;

const char *kl_audio_fmt_name(kl_audio_fmt fmt);   // "s16", "f32", ...
unsigned    kl_audio_fmt_bytes(kl_audio_fmt fmt);  // bytes per sample
// A bare bits-per-sample width -> a format. 0 on success, -1 for a width no
// integer encoding has. Signed at every width except 8, which is unsigned.
int         kl_audio_fmt_from_bits(unsigned bits, kl_audio_fmt *out);

// Open the device for a guest stream of `rate` Hz, `channels` channels, in
// `fmt`. Returns 0 on success; on failure the caller must keep its own clock.
// Idempotent for an identical format, reopens for a different one.
int  kl_audio_open_fmt(unsigned rate, unsigned channels, kl_audio_fmt fmt);

// The bits-per-sample spelling, for callers that only know a width. Signed
// integer is assumed at every width except 8, which is unsigned — that is what
// a bare width means to both SLDataFormat_PCM and AAudio's PCM_I*.
int  kl_audio_open(unsigned rate, unsigned channels, unsigned bits);

// Convert `n` interleaved samples of `fmt` into float32 in [-1, 1]. Exposed for
// tests; kl_audio_write applies it to every buffer before resampling.
void kl_audio_to_float(kl_audio_fmt fmt, const void *src, float *dst, size_t n);
void kl_audio_close(void);

// ---- more than one guest audio backend at a time ----
//
// A title can bring up two output paths and run BOTH. Tetris Effect does: UE4's
// own Android mixer takes an OpenSL ES buffer-queue player at 44100 Hz while
// Wwise takes an AAudio stream at 48000 Hz, and on hardware they were live
// together for the whole run on device (`[sl] audio player: 44100 Hz` and
// `[aaudio] openStream: 48000 Hz` twelve lines apart, 3,226 OpenSL buffers
// consumed beside 23,000 AAudio writes).
//
// Everything below this line in kl_audio.c was written for exactly ONE
// producer: one ring with a single-producer cursor, one resampler position and
// `g_prev`, one `realloc`'d scratch buffer. Two of them meant a racing write
// cursor, a scratch buffer reallocated under a thread reading it, each
// backend's PCM resampled at whichever ratio the other one's open() had last
// installed, and — because a ring that drains at 48 kHz cannot accept two
// 48 kHz streams — each producer blocked for half the time while the other's
// buffers were interleaved into the same output. It is audible as constant
// stutter.
//
// So a producer now identifies itself. The sink picks ONE owner — the first
// source that produces a non-silent buffer — and every other source is paced
// (so its mixer keeps its own clock and never wedges) and dropped. Ownership
// transfers if the owner falls silent for KL_AUDIO_STEAL_MS while a challenger
// is producing sound, which is what makes the choice recoverable rather than a
// coin flip taken at boot. KL_AUDIO_SOURCE=<name> forces it outright, and
// KL_AUDIO_MIX_SOURCES=1 restores the old free-for-all for an A/B.
//
// `tag` is any stable non-NULL pointer the backend owns (its player or stream
// object); `name` is for the log and for KL_AUDIO_SOURCE.
int    kl_audio_open_src_fmt(const void *tag, const char *name,
                             unsigned rate, unsigned channels, kl_audio_fmt fmt);
// ...and the bits-per-sample spelling of the same call.
int    kl_audio_open_src(const void *tag, const char *name,
                         unsigned rate, unsigned channels, unsigned bits);
void   kl_audio_close_src(const void *tag);
size_t kl_audio_write_src(const void *tag, const void *pcm, size_t bytes);
void   kl_audio_play_src(const void *tag);
void   kl_audio_pause_src(const void *tag);
void   kl_audio_flush_src(const void *tag);

// A producer backend's own report, appended to the sink's live one.
//
// The 15 s report says how many buffers arrived silent; only the backend that
// called the guest knows WHY — a dead-constant "1 in 6 buffers all-zero" says
// nothing on its own about whether the guest's mixer was late or the callback
// size did not divide its block. Registration is idempotent, so a backend may
// call it at every open.
void   kl_audio_reporter(void (*fn)(FILE *));

// Playback state. Stopping drops whatever is still queued, because the guest
// stops precisely when it is about to release the buffers it enqueued.
void kl_audio_play(void);
void kl_audio_pause(void);
void kl_audio_flush(void);

// Push one guest buffer. Converts, resamples and blocks until it fits inside
// the latency target — i.e. returns after roughly the buffer's own duration,
// paced by the audio device rather than by a sleep. Returns the number of
// bytes accepted; short only when the sink is stopping or wedged, which the
// caller should treat as "fall back to your own clock for this buffer".
size_t kl_audio_write(const void *pcm, size_t bytes);

// Same, but tagged with an opaque per-producer key so several producers can feed
// ONE device and be MIXED rather than corrupt each other's writes. Steam Link's
// VR client opens two output streams at once; each passes its stream pointer as
// `src`. kl_audio_write(pcm,bytes) is exactly this with src == NULL (one source).
size_t kl_audio_write_src(const void *src, const void *pcm, size_t bytes);

// True once a device is open and running, i.e. kl_audio_write is the clock.
int  kl_audio_active(void);

// visionOS: the OS takes the stream away — interruptions, route changes, the
// app being backgrounded out of an immersive space — and does not always give
// it back. Swift forwards the notifications here; the producer also watches
// for a render callback that has stopped arriving and restarts on its own, so
// a missed notification degrades to a ~0.5 s gap rather than to silence.
void kl_audio_interrupted(int began);
int  kl_audio_restart(void);
// The scene says the app is off screen — a deliberate silence, latching, so
// the watchdog's lost-notification escape cannot "recover" the stream back.
void kl_audio_set_silenced(int silenced);
// The app is back on screen — call it on any transition that can only mean
// that, whatever this side believes its own state to be.
//
// The case it exists for has no notification at all: a Digital Crown press
// temporarily dismisses the immersive space, CoreAudio stops calling the render
// callback, and the unit still reports itself started. ALVR hit the same bug
// and works around it the same way. The heartbeat above finds it too, but a
// second later and by finding silence that has already been heard; a compositor
// leaving `.paused` knows the moment exactly. The two overlap on purpose.
void kl_audio_resume(void);
// Swift calls this once the AVAudioSession is configured and active. Before it
// the hardware sample rate reads as a default rather than the real one, and an
// output unit initialised against the wrong rate fails at AudioUnitInitialize.
void kl_audio_session_ready(double sample_rate);

// How many times the render callback found the ring empty while the guest
// thought it was playing. Exposed because AAudio's AAudioStream_getXRunCount is
// exactly this number and FMOD polls it to size its own buffer — answering a
// constant 0 there is a claim that the device never underruns.
unsigned kl_audio_underruns(void);

// Milliseconds of audio queued ahead of the device for `tag`, or -1 if there is
// none to speak of (no device, stopped, interrupted, or `tag` is not the owner).
// A pull-side feeder uses this to decide whether it can afford to WAIT for its
// guest's next mixer block instead of writing a hole. See kl_audio.c.
double kl_audio_lead_ms(const void *tag);

void kl_audio_report(FILE *f);

// Raise the fill-target floor (ms) for a guest whose audio arrives over a
// jittery link (Steam Link's network stream), so a short stall is buffered
// rather than heard as a dropout. 0 leaves the 80 ms default. KL_AUDIO_LATENCY_MS
// still overrides. Set before the guest opens its audio device.
void kl_audio_set_latency_ms(unsigned ms);

// ---- the bounded PCM capture ----
//
// Counters cannot say what audio sounds like. Every counter here can come back
// clean — real-time pacing, no underruns after ownership, 0.0% of the owned
// output silent, a peak that is a running maximum and therefore says nothing
// about a given moment — while the output is still audibly wrong. A count
// cannot tell music from the same music with its channels swapped, its samples
// byte-reversed, its blocks spliced in the wrong order, or a mixer writing
// garbage. Only the samples can.
//
// So both ends of the path tee to a WAV: what the guest's callback handed us
// (kl_aaudio.c, guest rate, pre-resample) and what the render callback will
// hand the hardware (kl_audio.c, device rate, post-resample). Two files, so a
// difference between them attributes the damage to OUR pipeline and a
// similarity attributes it to the guest.
//
// BOUNDED and SELF-CLOSING, because the device's exit path is unreachable: the
// capture starts at the first buffer that carries signal, runs for
// kl_audio_dump_seconds(), then patches its own header and closes. A file that
// only gets written while a process is alive and only becomes valid when it
// dies is not evidence anyone can pull.
typedef struct kl_wav kl_wav;

// 16-bit PCM, interleaved, `ch` channels at `rate` Hz. `max_s` seconds is a
// hard cap — writes past it are dropped and the file closes itself. NULL if the
// path is unwritable, which every entry point below tolerates.
kl_wav *kl_wav_open(const char *path, unsigned rate, unsigned ch, double max_s);
// Append `frames` frames. Closes the file (and reports) on reaching the cap.
void    kl_wav_write_s16(kl_wav *w, const int16_t *pcm, size_t frames);
// ...and the float twin, clamped to [-1,1] and scaled — the sink's ring holds
// float and converting it back is cheaper than teeing before the conversion.
void    kl_wav_write_f32(kl_wav *w, const float *pcm, size_t frames);
void    kl_wav_close(kl_wav *w);

// KL_AUDIO_DUMP_S — how many seconds either capture records (default 20);
// 0 disables both. The captures themselves are off unless KL_AUDIO_DUMP /
// KL_AAUDIO_DUMP name a file.
double  kl_audio_dump_seconds(void);

// The same numbers as kl_audio_report, on one line and with no latch, so a run
// that never exits cleanly still says whether it made a sound. A port dies by
// being killed in the headset far more often than it dies by returning from
// main, and the end-of-run report has therefore never once been read on device.
void kl_audio_heartbeat(FILE *f);
// ---- Microphone capture (opt-in; OFF by default) ----
// The KleptonMic toggle (a persisted setting, mirrored on KleptonChroma) calls
// kl_audio_mic_set_enabled. Nothing touches the microphone — and so nothing
// triggers the visionOS mic permission prompt — while it is OFF: a guest input
// stream (kl_aaudio.c / kl_opensl.c) is still told "no capture device", exactly
// as before. When it is ON, a guest that opens an input stream drives
// kl_audio_mic_open/read/close and gets real samples.
void     kl_audio_mic_set_enabled(int on);
int      kl_audio_mic_enabled(void);

// Open the capture device (idempotent). Returns 0 on success; negative if the
// mic is disabled, unavailable, or permission was refused. want_rate/want_channels
// are hints; delivered samples are interleaved int16 at kl_audio_mic_rate() /
// kl_audio_mic_channels().
int      kl_audio_mic_open(unsigned want_rate, unsigned want_channels);
void     kl_audio_mic_close(void);
unsigned kl_audio_mic_rate(void);
unsigned kl_audio_mic_channels(void);

// Non-blocking: pull up to `frames` frames of interleaved int16 into buf.
// Returns frames actually read (0 if none buffered / not open / disabled).
int      kl_audio_mic_read_i16(int16_t *buf, int frames);

#endif
