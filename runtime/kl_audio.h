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
#include <stdio.h>

// Open the device for a guest stream of `rate` Hz, `channels` channels,
// `bits` bits per sample (16 is the only width FMOD produces and the only one
// implemented). Returns 0 on success; on failure the caller must keep its own
// clock. Idempotent for an identical format, reopens for a different one.
int  kl_audio_open(unsigned rate, unsigned channels, unsigned bits);
void kl_audio_close(void);

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

// True once a device is open and running, i.e. kl_audio_write is the clock.
int  kl_audio_active(void);

// visionOS: the OS takes the stream away — interruptions, route changes, the
// app being backgrounded out of an immersive space — and does not always give
// it back. Swift forwards the notifications here; the producer also watches
// for a render callback that has stopped arriving and restarts on its own, so
// a missed notification degrades to a ~0.5 s gap rather than to silence.
void kl_audio_interrupted(int began);
int  kl_audio_restart(void);
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

void kl_audio_report(FILE *f);

#endif
