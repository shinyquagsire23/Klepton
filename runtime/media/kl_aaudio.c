// See kl_aaudio.h. The guest-facing half of the audio path; kl_audio.c is the
// device.
//
// Two objects and one thread:
//
//   AAudioStreamBuilder  a plain settings bag. Every setter is void — AAudio
//                        validates at openStream, not at set time, so nothing
//                        here can fail early either.
//   AAudioStream         a builder's settings, frozen, plus the feeder thread
//                        that calls the guest's data callback and paces on
//                        kl_audio_write.
//
// The feeder is the whole design. AAudio pulls, so we are the one doing the
// calling, and the thing we call is guest code — kl_thread_init() first (S0.1)
// or the stack-protector prologue reads an empty TSD slot and the guest dies
// far from here.
#include "kl_aaudio.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_audio.h"

// ---------------------------------------------------------------------------
// The ABI's own numbers, transcribed from <aaudio/AAudio.h>. The guest tests
// these exactly — AAUDIO_OK is 0 and every error is a large negative — so they
// are contract, not preference. (The error block is written out rather than
// left as the header's chain of `= PREVIOUS + 2` arithmetic, because the gaps
// in that chain are reserved values and getting one wrong turns a refusal into
// a different refusal.)
enum { AAUDIO_OK = 0,
       AAUDIO_ERROR_DISCONNECTED     = -899,
       AAUDIO_ERROR_ILLEGAL_ARGUMENT = -898,
       AAUDIO_ERROR_INTERNAL         = -896,
       AAUDIO_ERROR_INVALID_STATE    = -895,
       AAUDIO_ERROR_INVALID_HANDLE   = -892,
       AAUDIO_ERROR_UNIMPLEMENTED    = -890,
       AAUDIO_ERROR_UNAVAILABLE      = -889,
       AAUDIO_ERROR_NO_FREE_HANDLES  = -888,
       AAUDIO_ERROR_NO_MEMORY        = -887,
       AAUDIO_ERROR_NULL             = -886,
       AAUDIO_ERROR_TIMEOUT          = -885,
       AAUDIO_ERROR_WOULD_BLOCK      = -884,
       AAUDIO_ERROR_INVALID_FORMAT   = -883,
       AAUDIO_ERROR_OUT_OF_RANGE     = -882,
       AAUDIO_ERROR_NO_SERVICE       = -881,
       AAUDIO_ERROR_INVALID_RATE     = -880 };

enum { AAUDIO_DIRECTION_OUTPUT = 0, AAUDIO_DIRECTION_INPUT = 1 };

enum { AAUDIO_FORMAT_INVALID       = -1,
       AAUDIO_FORMAT_UNSPECIFIED   = 0,
       AAUDIO_FORMAT_PCM_I16       = 1,
       AAUDIO_FORMAT_PCM_FLOAT     = 2,
       AAUDIO_FORMAT_PCM_I24_PACKED= 3,
       AAUDIO_FORMAT_PCM_I32       = 4 };

enum { AAUDIO_STREAM_STATE_UNINITIALIZED = 0, AAUDIO_STREAM_STATE_UNKNOWN = 1,
       AAUDIO_STREAM_STATE_OPEN = 2,     AAUDIO_STREAM_STATE_STARTING = 3,
       AAUDIO_STREAM_STATE_STARTED = 4,  AAUDIO_STREAM_STATE_PAUSING = 5,
       AAUDIO_STREAM_STATE_PAUSED = 6,   AAUDIO_STREAM_STATE_FLUSHING = 7,
       AAUDIO_STREAM_STATE_FLUSHED = 8,  AAUDIO_STREAM_STATE_STOPPING = 9,
       AAUDIO_STREAM_STATE_STOPPED = 10, AAUDIO_STREAM_STATE_CLOSING = 11,
       AAUDIO_STREAM_STATE_CLOSED = 12,  AAUDIO_STREAM_STATE_DISCONNECTED = 13 };

enum { AAUDIO_CALLBACK_RESULT_CONTINUE = 0, AAUDIO_CALLBACK_RESULT_STOP = 1 };

enum { AAUDIO_UNSPECIFIED = 0 };

typedef int32_t aaudio_result_t;

typedef int32_t (*klaa_data_cb)(void *stream, void *user, void *audio, int32_t frames);
typedef void    (*klaa_error_cb)(void *stream, void *user, aaudio_result_t err);

// ---------------------------------------------------------------------------
// The builder. Every field is "unspecified" until the guest says otherwise,
// which is the ABI's own default and the reason openStream substitutes.

typedef struct {
    int32_t direction, format, channels, rate, buffer_capacity;
    int32_t performance_mode, sharing_mode, input_preset;
    int32_t frames_per_data_cb, device_id;
    klaa_data_cb  data_cb;   void *data_user;
    klaa_error_cb error_cb;  void *error_user;
} klaa_builder;

typedef struct klaa_stream {
    int32_t rate, channels, format, burst;
    int32_t buffer_size;         // the guest's requested size, <= capacity
    // Carried from the builder so the getters can answer what was ADOPTED
    // rather than what was asked for. openStream substitutes for anything left
    // unspecified, and the substitution is the value in force — a getter that
    // replayed the builder's AAUDIO_UNSPECIFIED would describe a stream that
    // does not exist.
    int32_t direction, sharing_mode, performance_mode, input_preset, device_id;
    klaa_data_cb  data_cb;   void *data_user;
    klaa_error_cb error_cb;  void *error_user;

    pthread_mutex_t lock;
    int32_t state;
    int      running;            // the feeder's own loop flag
    pthread_t thread;
    int      thread_live;

    void    *scratch;            // what the guest's callback fills
    int16_t *staging;            // ...converted to the 16-bit kl_audio takes
    size_t   scratch_bytes, staging_bytes;

    unsigned long callbacks, frames;   // for the report
} klaa_stream;

// The device is a singleton (kl_audio.c owns one output unit), so a second
// simultaneous output stream would silently fight the first for it. Nothing
// measured does that; this counter is here so that if it ever happens the log
// says so rather than the sound going strange.
static int g_open_streams;
static unsigned long g_short_writes;
static int g_input_refusals;

static int klaa_burst_frames(void) {
    const char *e = getenv("KL_AAUDIO_BURST");
    int n = e ? atoi(e) : 0;
    // 240 frames is 5 ms at 48 kHz and is what this guest's own jitter buffer
    // is configured in multiples of. AAudio's real burst is the HAL's; the
    // guest only uses it to size its buffer, so any sane value works and a
    // small one keeps the callback rate close to Android's.
    if (n < 32 || n > 8192) n = 240;
    return n;
}

static int klaa_bytes_per_frame(const klaa_stream *s) {
    int w = (s->format == AAUDIO_FORMAT_PCM_FLOAT) ? 4 :
            (s->format == AAUDIO_FORMAT_PCM_I32)   ? 4 :
            (s->format == AAUDIO_FORMAT_PCM_I24_PACKED) ? 3 : 2;
    return w * s->channels;
}

// ---------------------------------------------------------------------------
// The feeder. One per started stream; the only thing on it besides guest code
// is a format conversion and the blocking write that paces it.

static void *klaa_feeder(void *arg) {
    klaa_stream *s = arg;
    kl_thread_init();                    // mandatory: the callback is guest code

    const int frame_in = klaa_bytes_per_frame(s);
    const int frame_out = 2 * s->channels;

    for (;;) {
        pthread_mutex_lock(&s->lock);
        int running = s->running;
        pthread_mutex_unlock(&s->lock);
        if (!running) break;

        int32_t frames = s->burst;
        memset(s->scratch, 0, (size_t)frames * frame_in);
        int32_t r = s->data_cb ? s->data_cb(s, s->data_user, s->scratch, frames)
                               : AAUDIO_CALLBACK_RESULT_STOP;
        s->callbacks++;
        s->frames += (unsigned long)frames;

        // Convert to the 16-bit PCM kl_audio takes. I16 is a straight handoff;
        // float is the other format this guest could plausibly ask for, and it
        // is converted here rather than in kl_audio.c because this is the
        // producer side, which is where every other conversion already lives.
        const void *pcm;
        if (s->format == AAUDIO_FORMAT_PCM_FLOAT) {
            const float *in = s->scratch;
            size_t n = (size_t)frames * s->channels;
            for (size_t i = 0; i < n; i++) {
                float v = in[i];
                if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
                s->staging[i] = (int16_t)lrintf(v * 32767.0f);
            }
            pcm = s->staging;
        } else {
            pcm = s->scratch;
        }

        size_t bytes = (size_t)frames * (size_t)frame_out;
        size_t played = kl_audio_write(pcm, bytes);
        if (played < bytes) {
            // No device, KL_AUDIO=0, or one that stopped draining: pace the
            // remainder ourselves, which is byte-for-byte what this loop would
            // be with no audio at all. Same fallback as kl_opensl.c's feeder.
            g_short_writes++;
            uint64_t rem = ((uint64_t)bytes - played) / (uint64_t)frame_out;
            useconds_t us = (useconds_t)(rem * 1000000ull / (uint64_t)s->rate);
            if (us) usleep(us > 100000 ? 100000 : us);
        }

        if (r == AAUDIO_CALLBACK_RESULT_STOP) {
            // The guest asking to stop from inside its own callback. Honour it
            // here rather than calling back into requestStop, which would join
            // this very thread.
            pthread_mutex_lock(&s->lock);
            s->running = 0;
            s->state = AAUDIO_STREAM_STATE_STOPPED;
            pthread_mutex_unlock(&s->lock);
            fprintf(stderr, "  [aaudio] data callback returned STOP; stream stopped\n");
            break;
        }
    }
    return NULL;
}

// Stop and join, with the lock dropped across the join — the feeder may be
// parked inside kl_audio_write, and kl_audio_pause() is what lets it out.
static void klaa_stop_join(klaa_stream *s) {
    pthread_mutex_lock(&s->lock);
    int live = s->thread_live;
    s->running = 0;
    if (s->state == AAUDIO_STREAM_STATE_STARTED ||
        s->state == AAUDIO_STREAM_STATE_STARTING)
        s->state = AAUDIO_STREAM_STATE_STOPPING;
    pthread_mutex_unlock(&s->lock);
    if (live) {
        kl_audio_pause();
        pthread_join(s->thread, NULL);
    }
    pthread_mutex_lock(&s->lock);
    s->thread_live = 0;
    s->state = AAUDIO_STREAM_STATE_STOPPED;
    pthread_mutex_unlock(&s->lock);
}

// ---------------------------------------------------------------------------
// AAudioStreamBuilder

static aaudio_result_t klaa_createStreamBuilder(klaa_builder **out) {
    if (!out) return AAUDIO_ERROR_NULL;
    klaa_builder *b = calloc(1, sizeof *b);
    if (!b) return AAUDIO_ERROR_NO_MEMORY;
    b->direction        = AAUDIO_DIRECTION_OUTPUT;
    b->format           = AAUDIO_FORMAT_UNSPECIFIED;
    b->performance_mode = 10;              // AAUDIO_PERFORMANCE_MODE_NONE
    b->sharing_mode     = 1;               // AAUDIO_SHARING_MODE_SHARED
    *out = b;
    return AAUDIO_OK;
}

static void klaa_setDirection(klaa_builder *b, int32_t v)        { if (b) b->direction = v; }
static void klaa_setPerformanceMode(klaa_builder *b, int32_t v)  { if (b) b->performance_mode = v; }
static void klaa_setSharingMode(klaa_builder *b, int32_t v)      { if (b) b->sharing_mode = v; }
static void klaa_setFormat(klaa_builder *b, int32_t v)           { if (b) b->format = v; }
static void klaa_setChannelCount(klaa_builder *b, int32_t v)     { if (b) b->channels = v; }
static void klaa_setSampleRate(klaa_builder *b, int32_t v)       { if (b) b->rate = v; }
static void klaa_setBufferCapacityInFrames(klaa_builder *b, int32_t v) { if (b) b->buffer_capacity = v; }
static void klaa_setInputPreset(klaa_builder *b, int32_t v)      { if (b) b->input_preset = v; }
static void klaa_setDeviceId(klaa_builder *b, int32_t v)         { if (b) b->device_id = v; }

// setFramesPerDataCallback is the guest asking for a FIXED callback size, and
// it is honoured rather than recorded: FMOD sets it to its own mixer block and
// then assumes every callback is exactly that many frames. Our burst is what
// the feeder passes, so the two must be the same number or FMOD mixes a block
// and submits a different one. AAUDIO_UNSPECIFIED means "you choose", which is
// the default we already had.
static void klaa_setFramesPerDataCallback(klaa_builder *b, int32_t v) {
    if (b) b->frames_per_data_cb = v;
}

static void klaa_setDataCallback(klaa_builder *b, klaa_data_cb cb, void *user) {
    if (b) { b->data_cb = cb; b->data_user = user; }
}
static void klaa_setErrorCallback(klaa_builder *b, klaa_error_cb cb, void *user) {
    if (b) { b->error_cb = cb; b->error_user = user; }
}

static aaudio_result_t klaa_builder_delete(klaa_builder *b) {
    free(b);
    return AAUDIO_OK;
}

static aaudio_result_t klaa_openStream(klaa_builder *b, klaa_stream **out) {
    if (!b || !out) return AAUDIO_ERROR_NULL;
    *out = NULL;

    if (b->direction == AAUDIO_DIRECTION_INPUT) {
        // Refused by design — see the header. This is the one place a "no" is
        // an answer rather than a gap, so it says so in full.
        g_input_refusals++;
        fprintf(stderr, "  [aaudio] openStream(INPUT, preset %d) -> UNAVAILABLE: "
                        "no capture device is presented (kl_aaudio.h)\n", b->input_preset);
        return AAUDIO_ERROR_UNAVAILABLE;
    }

    int32_t rate     = b->rate     > 0 ? b->rate     : 48000;
    int32_t channels = b->channels > 0 ? b->channels : 2;
    int32_t format   = b->format;
    if (format == AAUDIO_FORMAT_UNSPECIFIED || format == AAUDIO_FORMAT_INVALID)
        format = AAUDIO_FORMAT_PCM_I16;
    if (format != AAUDIO_FORMAT_PCM_I16 && format != AAUDIO_FORMAT_PCM_FLOAT) {
        fprintf(stderr, "  [aaudio] openStream: format %d is not implemented "
                        "(I16 and FLOAT are)\n", format);
        return AAUDIO_ERROR_INVALID_FORMAT;
    }
    if (channels < 1 || channels > 8) return AAUDIO_ERROR_OUT_OF_RANGE;

    klaa_stream *s = calloc(1, sizeof *s);
    if (!s) return AAUDIO_ERROR_NO_MEMORY;
    pthread_mutex_init(&s->lock, NULL);
    s->rate = rate; s->channels = channels; s->format = format;
    s->burst = b->frames_per_data_cb > 0 ? b->frames_per_data_cb : klaa_burst_frames();
    // Capacity is the guest's if it asked for one, and at least one burst
    // either way — a capacity below the callback size is a buffer that cannot
    // hold one callback's output.
    s->buffer_size = b->buffer_capacity > s->burst ? b->buffer_capacity : s->burst;
    s->data_cb = b->data_cb;   s->data_user = b->data_user;
    s->error_cb = b->error_cb; s->error_user = b->error_user;
    // The modes, resolved. Only OUTPUT is served (capture is refused by design,
    // above), so direction is not copied but ASSERTED — a stream that exists is
    // an output stream, whatever the builder said.
    s->direction        = AAUDIO_DIRECTION_OUTPUT;
    s->sharing_mode     = b->sharing_mode;
    s->performance_mode = b->performance_mode;
    s->input_preset     = b->input_preset;
    s->device_id        = b->device_id;
    s->state = AAUDIO_STREAM_STATE_OPEN;

    s->scratch_bytes = (size_t)s->burst * (size_t)klaa_bytes_per_frame(s);
    s->staging_bytes = (size_t)s->burst * 2u * (size_t)channels;
    s->scratch = calloc(1, s->scratch_bytes);
    s->staging = calloc(1, s->staging_bytes);
    if (!s->scratch || !s->staging) {
        free(s->scratch); free(s->staging); free(s);
        return AAUDIO_ERROR_NO_MEMORY;
    }

    if (g_open_streams)
        fprintf(stderr, "  [aaudio] NOTE: a second output stream is open; the "
                        "device is a singleton and they will share it\n");
    g_open_streams++;

    // Open the device now rather than at start, so a failure is visible where
    // the guest can still react to it. A failure is not fatal: the feeder
    // paces with usleep, which is the null-audio behaviour every earlier
    // measurement was taken against.
    kl_audio_open((unsigned)rate, (unsigned)channels, 16);

    fprintf(stderr, "  [aaudio] openStream: %d Hz, %d ch, %s, burst %d frames%s\n",
            rate, channels,
            format == AAUDIO_FORMAT_PCM_FLOAT ? "float" : "int16", s->burst,
            kl_audio_active() ? "" : " (no device — pacing with usleep)");
    *out = s;
    return AAUDIO_OK;
}

// ---------------------------------------------------------------------------
// AAudioStream

static aaudio_result_t klaa_requestStart(klaa_stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    pthread_mutex_lock(&s->lock);
    if (s->thread_live) { pthread_mutex_unlock(&s->lock); return AAUDIO_OK; }
    s->running = 1;
    s->state = AAUDIO_STREAM_STATE_STARTING;
    pthread_mutex_unlock(&s->lock);

    kl_audio_play();
    if (pthread_create(&s->thread, NULL, klaa_feeder, s) != 0) {
        pthread_mutex_lock(&s->lock);
        s->running = 0;
        s->state = AAUDIO_STREAM_STATE_OPEN;
        pthread_mutex_unlock(&s->lock);
        return AAUDIO_ERROR_INTERNAL;
    }
    pthread_mutex_lock(&s->lock);
    s->thread_live = 1;
    s->state = AAUDIO_STREAM_STATE_STARTED;
    pthread_mutex_unlock(&s->lock);
    return AAUDIO_OK;
}

static aaudio_result_t klaa_requestStop(klaa_stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    klaa_stop_join(s);
    kl_audio_flush();
    return AAUDIO_OK;
}

static aaudio_result_t klaa_close(klaa_stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    klaa_stop_join(s);
    fprintf(stderr, "  [aaudio] close: %lu callbacks, %lu frames\n",
            s->callbacks, s->frames);
    if (--g_open_streams <= 0) { g_open_streams = 0; kl_audio_close(); }
    pthread_mutex_destroy(&s->lock);
    free(s->scratch); free(s->staging); free(s);
    return AAUDIO_OK;
}

static int32_t klaa_getFramesPerBurst(klaa_stream *s) {
    return s ? s->burst : 0;
}

// ...and the read side of setFramesPerDataCallback above. Answered with the
// SAME number, which is the whole point of that setter's comment: the feeder
// passes s->burst frames to every data callback, so that is how many frames a
// callback actually gets — whether the guest named it or let us choose.
//
// AAUDIO_UNSPECIFIED (0) is the other defensible reading — the ABI describes
// this as "the value set on the builder" — and it is the dangerous one here. A
// guest that asked for nothing and is told 0 learns only that we did not
// promise; a guest that sizes a mixer block from it computes it from something
// other than the number it will be handed. Reporting the value actually in
// force is the same rule the buffer-size trio below is written to, and for the
// same reason.
static int32_t klaa_getFramesPerDataCallback(klaa_stream *s) {
    return s ? s->burst : 0;
}

// The property readbacks — one per builder setter, and they exist as a GROUP
// for the reason the display panel and the GLES capability set do: the guest
// configures a stream and then reads the configuration back, and answers that
// disagree with each other describe a device that cannot exist. FMOD does
// exactly this — it sets a rate and a channel count, opens, and then sizes its
// mixer from what it is told it got.
//
// Every one of these reports what openStream ADOPTED, never what the builder
// requested. Those differ whenever the guest left something unspecified, which
// is the common case, and the adopted value is the only one that describes the
// stream the guest now holds.
static int32_t klaa_getSampleRate(klaa_stream *s)      { return s ? s->rate : 0; }
static int32_t klaa_getChannelCount(klaa_stream *s)    { return s ? s->channels : 0; }
static int32_t klaa_getFormat(klaa_stream *s)          { return s ? s->format : 0; }
static int32_t klaa_getDirection(klaa_stream *s)       { return s ? s->direction : AAUDIO_DIRECTION_OUTPUT; }
static int32_t klaa_getSharingMode(klaa_stream *s)     { return s ? s->sharing_mode : 0; }
static int32_t klaa_getPerformanceMode(klaa_stream *s) { return s ? s->performance_mode : 0; }
static int32_t klaa_getInputPreset(klaa_stream *s)     { return s ? s->input_preset : 0; }

// getSamplesPerFrame is the OLD NAME for the channel count, not a second
// quantity — the two must never be allowed to drift apart, so it is the same
// function rather than a copy of its body.
#define klaa_getSamplesPerFrame klaa_getChannelCount

static int32_t klaa_getState(klaa_stream *s) {
    if (!s) return AAUDIO_STREAM_STATE_UNINITIALIZED;
    int32_t st;
    pthread_mutex_lock(&s->lock);
    st = s->state;
    pthread_mutex_unlock(&s->lock);
    return st;
}

// Frames written, which for an output stream is what the feeder has consumed
// from the guest. Read under the lock for the same reason the state is: the
// feeder thread advances it.
//
// int64 return, and that matters — these two are the only entry points here
// that are not int32, and truncating a frame counter is a fault that appears
// only after ~13 hours at 48 kHz.
static int64_t klaa_getFramesWritten(klaa_stream *s) {
    if (!s) return 0;
    int64_t n;
    pthread_mutex_lock(&s->lock);
    n = (int64_t)s->frames;
    pthread_mutex_unlock(&s->lock);
    return n;
}
// An output stream reads nothing. 0 is the count, not a refusal.
static int64_t klaa_getFramesRead(klaa_stream *s) { (void)s; return 0; }

// The buffer-size trio. FMOD tunes latency with these — it reads the capacity,
// sets a size inside it, and reads back what it actually got. Answering the
// capacity for both is not a stand-in: kl_audio.c's ring IS the buffer, the
// guest cannot resize it, and reporting a size we did not adopt would have
// FMOD compute a latency it does not have. Returning the value actually in
// force is what the ABI asks for — setBufferSizeInFrames returns the size it
// SETTLED on, not the one requested.
static int32_t klaa_getBufferCapacityInFrames(klaa_stream *s) {
    return s ? s->buffer_size : 0;
}
static int32_t klaa_getBufferSizeInFrames(klaa_stream *s) {
    return s ? s->buffer_size : 0;
}
static int32_t klaa_setBufferSizeInFrames(klaa_stream *s, int32_t frames) {
    if (!s) return AAUDIO_ERROR_NULL;
    if (frames < s->burst) frames = s->burst;               // clamped, as the HAL does
    if (frames > s->buffer_size) frames = s->buffer_size;
    return frames;
}

// AAUDIO_UNSPECIFIED is the ABI's own "no particular device", and it is the
// truth: kl_audio.c opens whatever CoreAudio calls the default output, and that
// is not a thing with an AAudio device id. A fabricated id would be one the
// guest could pass back to setDeviceId and get a different device for.
static int32_t klaa_getDeviceId(klaa_stream *s) { (void)s; return AAUDIO_UNSPECIFIED; }

// Underruns. kl_audio.c counts them for its own report; this is that number,
// because FMOD polls it to decide whether to grow its buffer — a hardcoded 0
// says "never underruns", which is a claim about a device we do not have.
static int32_t klaa_getXRunCount(klaa_stream *s) {
    (void)s;
    return (int32_t)kl_audio_underruns();
}

// MMAP is the low-latency shared-memory path into the Android audio HAL. There
// is no such path here — every frame goes through the ring in kl_audio.c — so
// this is false, and false is also what makes FMOD choose its ordinary timing
// model rather than one built on a promise we cannot keep.
static int32_t klaa_isMMapUsed(klaa_stream *s) { (void)s; return 0; }

// The guest's idiom is "ask to start, then wait for STARTING to become
// something else". Our start is synchronous, so the state has already moved
// and this returns immediately — which is the same answer a fast HAL gives.
static aaudio_result_t klaa_waitForStateChange(klaa_stream *s, int32_t from,
                                               int32_t *next, int64_t timeout_ns) {
    if (!s) return AAUDIO_ERROR_NULL;
    const int64_t step_ns = 2 * 1000 * 1000;
    int64_t waited = 0;
    for (;;) {
        pthread_mutex_lock(&s->lock);
        int32_t now = s->state;
        pthread_mutex_unlock(&s->lock);
        if (now != from) { if (next) *next = now; return AAUDIO_OK; }
        if (waited >= timeout_ns) { if (next) *next = now; return AAUDIO_ERROR_TIMEOUT; }
        usleep((useconds_t)(step_ns / 1000));
        waited += step_ns;
    }
}

static const char *klaa_convertResultToText(aaudio_result_t r) {
    switch (r) {
    case AAUDIO_OK:                    return "AAUDIO_OK";
    case AAUDIO_ERROR_DISCONNECTED:    return "AAUDIO_ERROR_DISCONNECTED";
    case AAUDIO_ERROR_ILLEGAL_ARGUMENT:return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case AAUDIO_ERROR_INTERNAL:        return "AAUDIO_ERROR_INTERNAL";
    case AAUDIO_ERROR_INVALID_STATE:   return "AAUDIO_ERROR_INVALID_STATE";
    case AAUDIO_ERROR_INVALID_HANDLE:  return "AAUDIO_ERROR_INVALID_HANDLE";
    case AAUDIO_ERROR_UNIMPLEMENTED:   return "AAUDIO_ERROR_UNIMPLEMENTED";
    case AAUDIO_ERROR_UNAVAILABLE:     return "AAUDIO_ERROR_UNAVAILABLE";
    case AAUDIO_ERROR_NO_FREE_HANDLES: return "AAUDIO_ERROR_NO_FREE_HANDLES";
    case AAUDIO_ERROR_NO_MEMORY:       return "AAUDIO_ERROR_NO_MEMORY";
    case AAUDIO_ERROR_NULL:            return "AAUDIO_ERROR_NULL";
    case AAUDIO_ERROR_TIMEOUT:         return "AAUDIO_ERROR_TIMEOUT";
    case AAUDIO_ERROR_WOULD_BLOCK:     return "AAUDIO_ERROR_WOULD_BLOCK";
    case AAUDIO_ERROR_INVALID_FORMAT:  return "AAUDIO_ERROR_INVALID_FORMAT";
    case AAUDIO_ERROR_OUT_OF_RANGE:    return "AAUDIO_ERROR_OUT_OF_RANGE";
    case AAUDIO_ERROR_NO_SERVICE:      return "AAUDIO_ERROR_NO_SERVICE";
    case AAUDIO_ERROR_INVALID_RATE:    return "AAUDIO_ERROR_INVALID_RATE";
    default:                           return "AAUDIO_ERROR_UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// The import door.

typedef struct { const char *name; void *fn; } klaa_entry;
#define A(n, f) { n, (void *)(f) }
static const klaa_entry g_aaudio[] = {
    A("AAudio_createStreamBuilder",                klaa_createStreamBuilder),
    A("AAudio_convertResultToText",                klaa_convertResultToText),
    A("AAudioStreamBuilder_setDirection",          klaa_setDirection),
    A("AAudioStreamBuilder_setPerformanceMode",    klaa_setPerformanceMode),
    A("AAudioStreamBuilder_setSharingMode",        klaa_setSharingMode),
    A("AAudioStreamBuilder_setFormat",             klaa_setFormat),
    A("AAudioStreamBuilder_setChannelCount",       klaa_setChannelCount),
    A("AAudioStreamBuilder_setSampleRate",         klaa_setSampleRate),
    A("AAudioStreamBuilder_setBufferCapacityInFrames", klaa_setBufferCapacityInFrames),
    A("AAudioStreamBuilder_setInputPreset",        klaa_setInputPreset),
    A("AAudioStreamBuilder_setDataCallback",       klaa_setDataCallback),
    A("AAudioStreamBuilder_setErrorCallback",      klaa_setErrorCallback),
    A("AAudioStreamBuilder_openStream",            klaa_openStream),
    A("AAudioStreamBuilder_delete",                klaa_builder_delete),
    A("AAudioStream_requestStart",                 klaa_requestStart),
    A("AAudioStream_requestStop",                  klaa_requestStop),
    A("AAudioStream_close",                        klaa_close),
    A("AAudioStream_getFramesPerBurst",            klaa_getFramesPerBurst),
    A("AAudioStream_getFramesPerDataCallback",     klaa_getFramesPerDataCallback),
    A("AAudioStream_getSampleRate",                klaa_getSampleRate),
    A("AAudioStream_getChannelCount",              klaa_getChannelCount),
    A("AAudioStream_getSamplesPerFrame",           klaa_getSamplesPerFrame),
    A("AAudioStream_getFormat",                    klaa_getFormat),
    A("AAudioStream_getDirection",                 klaa_getDirection),
    A("AAudioStream_getSharingMode",               klaa_getSharingMode),
    A("AAudioStream_getPerformanceMode",           klaa_getPerformanceMode),
    A("AAudioStream_getInputPreset",               klaa_getInputPreset),
    A("AAudioStream_getState",                     klaa_getState),
    A("AAudioStream_getFramesWritten",             klaa_getFramesWritten),
    A("AAudioStream_getFramesRead",                klaa_getFramesRead),
    A("AAudioStreamBuilder_setFramesPerDataCallback", klaa_setFramesPerDataCallback),
    A("AAudioStreamBuilder_setDeviceId",           klaa_setDeviceId),
    A("AAudioStream_waitForStateChange",           klaa_waitForStateChange),
    A("AAudioStream_getBufferCapacityInFrames",    klaa_getBufferCapacityInFrames),
    A("AAudioStream_getBufferSizeInFrames",        klaa_getBufferSizeInFrames),
    A("AAudioStream_setBufferSizeInFrames",        klaa_setBufferSizeInFrames),
    A("AAudioStream_getDeviceId",                  klaa_getDeviceId),
    A("AAudioStream_getXRunCount",                 klaa_getXRunCount),
    A("AAudioStream_isMMapUsed",                   klaa_isMMapUsed),
};
#undef A

void *kl_aaudio_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_aaudio / sizeof g_aaudio[0]; i++)
        if (!strcmp(g_aaudio[i].name, name)) return g_aaudio[i].fn;
    return NULL;                // must be able to say no — see the header
}

// ...and the DLOPEN door, which is a second one and not the same one.
//
// Steam Link DT_NEEDEDs libaaudio.so, so its nineteen names bind at relocation
// time through kl_aaudio_lookup above and no file is ever opened. Unity does
// not: FMOD dlopen()s "libaaudio.so" by name at output-device selection and
// resolves each entry point with dlsym. There is no such file in a guest tree,
// so that fell through to the ELF loader and failed — and FMOD's answer to a
// NULL handle is to give up on the device entirely:
//
//   E/Unity: FMOD failed to initialize the output device.: (60)
//
// which is a SILENT loss of all audio, reported once, several layers from the
// dlopen that caused it. Same reasoning as the libGLESv2/libvulkan lines in
// klb_dlopen: there is nothing on disk to fall through TO, so the synthetic
// handle has to come first.
// Abort BY NAME rather than at address zero — the project's standing rule for
// the difference between "we do not serve this" (a NULL from the lookup above,
// which a prober is entitled to see) and "the guest called it anyway".
static uint64_t klaa_unimplemented(const char *name) {
    fprintf(stderr, "\n[klepton] fatal: guest called unimplemented AAudio entry "
                    "point '%s'\n", name);
    kl_fatal_prepare();
    abort();
}

static const char g_aa_handle[] = "klepton-aaudio";

int kl_aaudio_claims(const char *soname) {
    if (!soname) return 0;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    return strcmp(b, "libaaudio.so") == 0;
}

void *kl_aaudio_dlopen(const char *soname) {
    if (!kl_aaudio_claims(soname)) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    fprintf(stderr, "  [aaudio] guest dlopen(\"%s\") -> synthetic AAudio handle\n", b);
    return (void *)g_aa_handle;
}

int kl_aaudio_is_handle(const void *h) { return h == (const void *)g_aa_handle; }

// dlsym on that handle. Unlike the import door this must NOT answer NULL for a
// name we do not serve: FMOD probes for entry points it can do without, and a
// NULL is how it learns that — but a name it then CALLS must fail by name
// rather than at address zero. kl_named_stub is that distinction, and it is the
// same choice kl_opensl_sym makes.
void *kl_aaudio_sym(const char *name) {
    if (!name) return NULL;
    void *fn = kl_aaudio_lookup(name);
    if (fn) return fn;
    if (strncmp(name, "AAudio", 6) != 0) return NULL;   // not ours to answer for
    return kl_named_stub(name, (void *)klaa_unimplemented);
}

void kl_aaudio_report(FILE *f) {
    if (!g_open_streams && !g_short_writes && !g_input_refusals) return;
    fprintf(f, "  [aaudio] %d stream(s) open, %lu short writes, %d input refusal(s)\n",
            g_open_streams, g_short_writes, g_input_refusals);
}
