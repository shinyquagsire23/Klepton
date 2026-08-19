// See kl_audio.h. The producer/consumer split, in one paragraph:
//
//   feeder thread (kl_opensl.c)          CoreAudio render thread
//   ----------------------------          -----------------------
//   pop guest buffer                      memcpy out of the ring
//   int16 -> float32, N ch -> out_ch      advance the read cursor
//   resample guest rate -> device rate    (nothing else, ever)
//   push into the ring, block if full
//   call the guest's buffer-queue cb
//
// The ring is single-producer/single-consumer with monotonic frame counters, so
// neither side needs a lock and the render callback is genuinely lock-free. The
// producer blocks by polling, not by waiting on a condvar, because signalling a
// condvar from the render callback is exactly the kind of thing that is fine
// until the day it is not.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <TargetConditionals.h>
#include <AudioToolbox/AudioToolbox.h>

#include "klepton.h"
#include "kl_audio.h"

#define KL_AUDIO_MAX_CH 2

// ---- state ----
static AudioUnit    g_unit;
static int          g_open;           // a unit exists and is initialised
static int          g_running;        // ...and AudioOutputUnitStart succeeded
static int          g_playing;        // the guest wants sound
static int          g_interrupted;    // the OS took the stream
static uint64_t     g_interrupt_ns;   // ...when, so a lost `.ended` is escapable
static unsigned     g_interrupt_max_ms = 1500;
// The recovery machinery lives at the bottom of the file, beside the producer
// it used to be part of; kl_audio_open and kl_audio_interrupted are above it.
static unsigned     g_wd_streak;      // consecutive restarts, for the backoff
static void         watchdog_start(void);
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   // unit lifecycle only

static unsigned g_in_rate, g_in_ch, g_in_bits;   // the guest's format
static unsigned g_out_ch = 2;
static double   g_out_rate;                      // the device's, measured
static double   g_session_rate;                  // what Swift told us, if anything

// The ring. Indices are monotonic frame counts; the modulo happens at use, so
// there is no full/empty ambiguity and no wrap flag to get wrong.
static float           *g_ring;
static size_t           g_cap;          // frames
static size_t           g_target;       // frames of fill the producer aims for
static _Atomic size_t   g_w, g_r;

// Multiple producers, MIXED. Steam Link's VR client opens TWO output streams at
// once (a float one and an int16 one, both AAudio, both 48 kHz stereo after the
// feeder's conversion) and each has its own feeder thread calling in here. The
// ring is single-producer by construction, so two of them appending raced the
// write cursor and interleaved their audio — heard as the wild, unfixable
// stutter that no buffer touched. So writes now MIX (add) into the ring at a
// per-source position kept in step with the reader, and the render callback
// ZEROES each slot as it consumes it so the next pass sums from silence. One
// source is the old behaviour exactly (add into a zeroed slot == a copy). A
// small lock serialises the producers (never the reader): they run on feeder
// threads that can afford to block, the reader cannot.
#define KL_AU_MAX_SRC 4
static const void      *g_src_key[KL_AU_MAX_SRC];
static uint64_t         g_src_pos[KL_AU_MAX_SRC];   // next write frame, per source
static int              g_src_n;
static pthread_mutex_t  g_wlock = PTHREAD_MUTEX_INITIALIZER;

// Slot for a producer key (NULL = the single-source/OpenSL path -> slot 0).
// Caller holds g_wlock. Overflow folds into slot 0 rather than dropping audio.
static int src_slot(const void *key) {
    if (!key) return 0;
    for (int i = 0; i < g_src_n; i++) if (g_src_key[i] == key) return i;
    if (g_src_n < KL_AU_MAX_SRC) { g_src_key[g_src_n] = key; g_src_pos[g_src_n] = 0; return g_src_n++; }
    return 0;
}

// A driver-set floor for the fill target, in ms (0 = none). See kl_audio_open.
static unsigned         g_latency_override_ms;
void kl_audio_set_latency_ms(unsigned ms) { g_latency_override_ms = ms; }

// Resampler state, producer-side only.
static double g_pos;                     // position in the input stream
static float  g_prev[KL_AUDIO_MAX_CH];   // last input frame of the last chunk
static float *g_scratch;                 // converted+resampled staging
static size_t g_scratch_frames;

// Counters, for the report and for the watchdog.
static _Atomic uint64_t g_last_render_ns;
static _Atomic uint64_t g_frames_played, g_frames_silent;
static _Atomic uint32_t g_underruns;
static unsigned long    g_writes, g_short_writes, g_restarts;
static int              g_trace;
// The peak sample the guest ever handed us. A buffer count says the mixer ran;
// this says the mixer produced audio — the same distinction `make check` draws
// for the opus roundtrip, and the one that would have caught "3,238 buffers
// consumed, nothing heard" years earlier.
static int              g_peak;
static unsigned long    g_silent_buffers;

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// ---- KL_AUDIO_DUMP — the capture, and the reason it is worth having ----
//
// "Sound" is the one output of this runtime that cannot be checked by reading a
// log. A buffer count, a peak level and an underrun count together say the
// plumbing is right; they cannot distinguish music from a resampler running at
// the wrong ratio, channels swapped, or a ring being read one frame behind
// itself — all of which are audible and none of which are visible. So the
// device path can also tee to a WAV, exactly the frames the render callback
// will hand the hardware, and that file is playable evidence. The GL twin is
// KL_GLFB_OUT, and it earned its keep for the same reason.
//
// What it records is what the producer accepted, contiguously — so a pause is
// elided rather than rendered as silence, and the file is shorter than the run.
// That is deliberate: the question this answers is "is what we send correct",
// and silence between tracks is not evidence either way.
static FILE  *g_dump;
static size_t g_dump_frames;

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void dump_open(void) {
    const char *path = kl_env_str("KL_AUDIO_DUMP", NULL);
    if (!path || !*path) return;
    g_dump = fopen(path, "wb");
    if (!g_dump) { fprintf(stderr, "  [au] cannot write %s\n", path); return; }
    g_dump_frames = 0;
    // Sizes are patched in dump_close(); a run that dies without closing leaves
    // a header claiming zero, which most players read as an empty file. Better
    // than a header claiming a length the data does not have.
    fwrite("RIFF", 1, 4, g_dump); put32(g_dump, 0); fwrite("WAVEfmt ", 1, 8, g_dump);
    put32(g_dump, 16); put16(g_dump, 1); put16(g_dump, (uint16_t)g_out_ch);
    put32(g_dump, (uint32_t)g_out_rate);
    put32(g_dump, (uint32_t)(g_out_rate * g_out_ch * 2));
    put16(g_dump, (uint16_t)(g_out_ch * 2)); put16(g_dump, 16);
    fwrite("data", 1, 4, g_dump); put32(g_dump, 0);
    fprintf(stderr, "  [au] capturing the device stream to %s\n", path);
}

static void dump_write(const float *frames, size_t n) {
    if (!g_dump || !n) return;
    for (size_t i = 0; i < n * g_out_ch; i++) {
        float v = frames[i];
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        put16(g_dump, (uint16_t)(int16_t)lrintf(v * 32767.0f));
    }
    g_dump_frames += n;
}

static void dump_close(void) {
    if (!g_dump) return;
    uint32_t bytes = (uint32_t)(g_dump_frames * g_out_ch * 2);
    fseek(g_dump, 4, SEEK_SET);  put32(g_dump, 36 + bytes);
    fseek(g_dump, 40, SEEK_SET); put32(g_dump, bytes);
    fclose(g_dump);
    g_dump = NULL;
    fprintf(stderr, "  [au] capture closed: %.2f s\n",
            g_out_rate > 0 ? (double)g_dump_frames / g_out_rate : 0.0);
}

static void nap_ms(unsigned ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// ---- the render callback ----
//
// Everything here is a load, a memcpy and a store. No locks, no allocation, no
// guest code — the guest's callback runs on the feeder thread and always has.
static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *flags,
                          const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                          AudioBufferList *io) {
    (void)ref; (void)ts; (void)bus;
    atomic_store_explicit(&g_last_render_ns, now_ns(), memory_order_relaxed);

    if (!io || io->mNumberBuffers < 1) return noErr;
    float *dst = (float *)io->mBuffers[0].mData;
    size_t want = nframes;
    if (!dst || !g_ring) {
        if (dst) memset(dst, 0, io->mBuffers[0].mDataByteSize);
        if (flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
        return noErr;
    }

    size_t r = atomic_load_explicit(&g_r, memory_order_relaxed);
    size_t w = atomic_load_explicit(&g_w, memory_order_acquire);
    size_t have = w - r;
    size_t take = have < want ? have : want;

    for (size_t done = 0; done < take; ) {
        size_t off = (r + done) % g_cap;
        size_t run = g_cap - off;
        if (run > take - done) run = take - done;
        memcpy(dst + done * g_out_ch, g_ring + off * g_out_ch,
               run * g_out_ch * sizeof(float));
        // Zero what we just took: the ring is a mix accumulator now, and a slot
        // reused on the next wrap must start from silence so producers can add
        // into it. Harmless for the single-source case (it re-zeros a slot the
        // one producer will fully overwrite anyway).
        memset(g_ring + off * g_out_ch, 0, run * g_out_ch * sizeof(float));
        done += run;
    }
    if (take < want) {
        memset(dst + take * g_out_ch, 0, (want - take) * g_out_ch * sizeof(float));
        atomic_fetch_add_explicit(&g_frames_silent, want - take, memory_order_relaxed);
        // An underrun while the guest thinks it is playing is a real dropout;
        // one while it is stopped is just an idle device, and counting those
        // would bury the signal under thousands of them.
        if (take == 0 && !g_playing) {
            if (flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
        } else {
            atomic_fetch_add_explicit(&g_underruns, 1, memory_order_relaxed);
        }
    }
    atomic_fetch_add_explicit(&g_frames_played, take, memory_order_relaxed);
    atomic_store_explicit(&g_r, r + take, memory_order_release);
    return noErr;
}

// ---- the unit ----
static const char *fourcc(OSStatus s, char buf[8]) {
    uint32_t v = (uint32_t)s;
    unsigned char c[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 8), (unsigned char)v};
    for (int i = 0; i < 4; i++)
        if (c[i] < 0x20 || c[i] > 0x7e) { snprintf(buf, 8, "%d", (int)s); return buf; }
    snprintf(buf, 8, "'%c%c%c%c'", c[0], c[1], c[2], c[3]);
    return buf;
}

#define CHECK(call, what) do { \
    OSStatus _s = (call); \
    if (_s != noErr) { char _b[8]; \
        fprintf(stderr, "  [au] %s failed: %s\n", (what), fourcc(_s, _b)); \
        goto fail; } } while (0)

// Ask the unit what the hardware is actually doing. On RemoteIO the output
// scope of element 0 IS the hardware side, and it will refuse to initialise if
// our client rate disagrees with it — there is no sample-rate converter in
// there. Hence a resampler on our side rather than a hopeful SetProperty.
static double measure_device_rate(AudioUnit u) {
    AudioStreamBasicDescription hw;
    UInt32 sz = sizeof hw;
    if (AudioUnitGetProperty(u, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output, 0, &hw, &sz) == noErr &&
        hw.mSampleRate > 8000.0 && hw.mSampleRate < 400000.0)
        return hw.mSampleRate;
    return 0.0;
}

static int unit_create(void) {
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_OSX
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
#else
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
#endif
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) { fprintf(stderr, "  [au] no output AudioComponent\n"); return -1; }
    CHECK(AudioComponentInstanceNew(comp, &g_unit), "AudioComponentInstanceNew");

    double hw = measure_device_rate(g_unit);
    if (hw <= 0.0) hw = g_session_rate > 0.0 ? g_session_rate : (double)g_in_rate;
    g_out_rate = hw;

    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate       = g_out_rate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = g_out_ch;
    fmt.mFramesPerPacket  = 1;
    fmt.mBitsPerChannel   = 32;
    fmt.mBytesPerFrame    = (UInt32)(sizeof(float) * g_out_ch);
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;
    CHECK(AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &fmt, sizeof fmt),
          "SetProperty(StreamFormat)");

    AURenderCallbackStruct cb = {render_cb, NULL};
    CHECK(AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &cb, sizeof cb),
          "SetProperty(SetRenderCallback)");

    CHECK(AudioUnitInitialize(g_unit), "AudioUnitInitialize");
    return 0;
fail:
    if (g_unit) { AudioComponentInstanceDispose(g_unit); g_unit = NULL; }
    return -1;
}

static void unit_destroy(void) {
    if (!g_unit) return;
    if (g_running) AudioOutputUnitStop(g_unit);
    g_running = 0;
    AudioUnitUninitialize(g_unit);
    AudioComponentInstanceDispose(g_unit);
    g_unit = NULL;
}

static int unit_start(void) {
    if (!g_unit || g_running) return 0;
    OSStatus s = AudioOutputUnitStart(g_unit);
    if (s != noErr) {
        char b[8];
        fprintf(stderr, "  [au] AudioOutputUnitStart failed: %s\n", fourcc(s, b));
        return -1;
    }
    g_running = 1;
    atomic_store_explicit(&g_last_render_ns, now_ns(), memory_order_relaxed);
    return 0;
}

// ---- open / close ----
int kl_audio_open(unsigned rate, unsigned channels, unsigned bits) {
    if (!kl_env_on("KL_AUDIO", 1)) {
        static int said;
        if (!said++) fprintf(stderr, "  [au] KL_AUDIO=0 — no device, the feeder keeps its own clock\n");
        return -1;
    }
    // 16-bit is what FMOD produces and the only width the converter below
    // implements. Anything else is refused loudly rather than played as noise.
    if (bits != 16 || channels < 1 || channels > 8 || rate < 8000 || rate > 192000) {
        fprintf(stderr, "  [au] refusing guest format %u Hz / %u ch / %u bit "
                        "(only 16-bit is implemented)\n", rate, channels, bits);
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    if (g_open && rate == g_in_rate && channels == g_in_ch && bits == g_in_bits) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (g_open) unit_destroy();
    g_open = 0;

    g_in_rate = rate; g_in_ch = channels; g_in_bits = bits;
    g_out_ch = channels == 1 ? 1 : 2;
    g_trace = kl_env_on("KL_AUDIO_TRACE", 0);

    if (unit_create() != 0) { pthread_mutex_unlock(&g_lock); return -1; }

    // The ring holds a full second so a scheduling hiccup on either side cannot
    // wrap it; the *fill target* is what sets latency, and it is much smaller.
    // Keeping those two separate is what lets the target grow to fit an
    // unexpectedly large guest buffer without reallocating anything.
    // 80 ms is fine for a LOCAL mixer (FMOD), which never starves its own feed.
    // A streamed guest (Steam Link) delivers audio over the network in bursts,
    // and a stall shorter than the fill target is silently absorbed while a
    // longer one is a dropout — so a driver that knows its audio arrives over a
    // jittery link raises this floor (kl_audio_set_latency_ms). KL_AUDIO_LATENCY_MS
    // still overrides both.
    unsigned lat_ms = kl_env_int("KL_AUDIO_LATENCY_MS",
                                 g_latency_override_ms ? g_latency_override_ms : 80);
    if (lat_ms < 10) lat_ms = 10;
    if (lat_ms > 500) lat_ms = 500;

    free(g_ring);
    g_cap = (size_t)g_out_rate + 1;
    g_ring = calloc(g_cap * g_out_ch, sizeof(float));
    free(g_scratch);
    g_scratch_frames = 0;
    g_scratch = NULL;
    if (!g_ring) { unit_destroy(); pthread_mutex_unlock(&g_lock); return -1; }

    g_target = (size_t)(g_out_rate * lat_ms / 1000.0);
    atomic_store(&g_w, 0); atomic_store(&g_r, 0);
    g_pos = 0.0;
    memset(g_prev, 0, sizeof g_prev);
    g_open = 1;
    g_interrupted = 0;
    // Once per process, not once per open: FMOD destroys and re-creates the
    // player, and a capture that restarted with it would silently keep only the
    // last few seconds of a long run.
    static int dump_tried;
    if (!dump_tried++) dump_open();

    g_interrupt_ns = now_ns();
    // Shorter than the original 3 s, but NOT so short it fires before the audio
    // session has finished reactivating. The common interruption here is the
    // shell window closing as the immersive space takes over, whose ".ended" this
    // OS routinely drops; the session comes back on its own ~1–1.5 s later. Fire
    // BEFORE that and the restart lands on a still-dead session, plays silence,
    // and the heartbeat restarts again — several short jumps instead of one gap
    // (600 ms did exactly that). 1.5 s lands just past the reactivation, so the
    // single escape restart takes and the handoff recovers in one step.
    // KL_AUDIO_INTERRUPT_MAX_MS overrides — lower it if your recovery is faster.
    g_interrupt_max_ms = kl_env_int("KL_AUDIO_INTERRUPT_MAX_MS", 1500);
    // Start the unit immediately, even with nothing queued. A stopped unit is
    // one the OS is free to tear down harder than a running silent one, and the
    // render callback's timestamp is the watchdog's only heartbeat.
    unit_start();
    pthread_mutex_unlock(&g_lock);
    watchdog_start();

    fprintf(stderr, "  [au] CoreAudio out: guest %u Hz/%u ch -> device %.0f Hz/%u ch, "
                    "%u ms target%s\n",
            rate, channels, g_out_rate, g_out_ch, lat_ms,
            fabs(g_out_rate - (double)rate) > 1.0 ? " (resampling)" : "");
    return 0;
}

void kl_audio_close(void) {
    pthread_mutex_lock(&g_lock);
    g_playing = 0;
    unit_destroy();
    // NOT dump_close(): FMOD destroys and re-creates the player during startup,
    // so closing here ended the capture 0.12 s into a 12.8 s run. The capture is
    // per-process and the report closes it.
    g_open = 0;
    free(g_ring);   g_ring = NULL;   g_cap = 0;
    free(g_scratch); g_scratch = NULL; g_scratch_frames = 0;
    atomic_store(&g_w, 0); atomic_store(&g_r, 0);
    pthread_mutex_unlock(&g_lock);
}

int kl_audio_active(void) { return g_open && g_running; }

void kl_audio_play(void) {
    if (!g_open) return;
    g_playing = 1;
    pthread_mutex_lock(&g_lock);
    unit_start();
    pthread_mutex_unlock(&g_lock);
}

void kl_audio_pause(void) { g_playing = 0; }

void kl_audio_flush(void) {
    if (!g_open) return;
    // The read cursor belongs to the render thread, so it cannot simply be
    // assigned from here: a callback that had already loaded `r` would store
    // r+take afterwards and rewind the ring over stale audio. AudioOutputUnitStop
    // blocks until the callback has returned, which is the only cheap way to get
    // exclusive ownership of that cursor. Flushes happen on a play-state change,
    // so the cost is not on any hot path.
    pthread_mutex_lock(&g_lock);
    int was = g_running;
    if (was && g_unit) { AudioOutputUnitStop(g_unit); g_running = 0; }
    atomic_store(&g_r, atomic_load(&g_w));
    g_pos = 0.0;
    memset(g_prev, 0, sizeof g_prev);
    if (was) unit_start();
    pthread_mutex_unlock(&g_lock);
}

// ---- the OS taking the stream away ----
void kl_audio_interrupted(int began) {
    fprintf(stderr, "  [au] session interruption %s\n", began ? "began" : "ended");
    g_interrupt_ns = now_ns();
    if (began) {
        g_interrupted = 1;
        pthread_mutex_lock(&g_lock);
        if (g_unit && g_running) { AudioOutputUnitStop(g_unit); g_running = 0; }
        pthread_mutex_unlock(&g_lock);
    } else {
        g_interrupted = 0;
        g_wd_streak = 0;
        kl_audio_restart();
    }
}

// Rebuild the unit. Used by both the notification path and the watchdog, and it
// has to be a full rebuild rather than a stop/start: a route change can leave
// the unit initialised against a sample rate the hardware no longer has, and
// that unit will start successfully and never call back.
int kl_audio_restart(void) {
    if (!g_open) return -1;
    pthread_mutex_lock(&g_lock);
    unit_destroy();
    int rc = unit_create();
    if (rc == 0) {
        // A rate change means the ring's contents are at the wrong rate; drop
        // them rather than play them slow. Latency target scales with the rate.
        if (g_cap != (size_t)g_out_rate + 1) {
            size_t frames = (size_t)g_out_rate + 1;
            float *nr = calloc(frames * g_out_ch, sizeof(float));
            if (nr) {
                free(g_ring); g_ring = nr;
                g_target = (size_t)((double)g_target * g_out_rate / (double)(g_cap - 1));
                g_cap = frames;
            }
        }
        atomic_store(&g_r, atomic_load(&g_w));
        g_pos = 0.0;
        memset(g_prev, 0, sizeof g_prev);
        unit_start();
    }
    pthread_mutex_unlock(&g_lock);
    g_restarts++;
    fprintf(stderr, "  [au] restart %s (device now %.0f Hz)\n",
            rc == 0 ? "ok" : "FAILED", g_out_rate);
    return rc;
}

void kl_audio_session_ready(double sample_rate) {
    g_session_rate = sample_rate;
    fprintf(stderr, "  [au] audio session ready, %.0f Hz\n", sample_rate);
    // If a unit already exists it was built against a guess; rebuild it against
    // the real rate. Before the session is active the hardware format reads as
    // a default, and a unit initialised against the wrong rate is the failure
    // that presents as "everything succeeded and nothing is audible".
    if (g_open && fabs(sample_rate - g_out_rate) > 1.0) kl_audio_restart();
}

// ---- the producer ----
//
// int16 interleaved at the guest rate -> float32 interleaved at the device
// rate, with linear interpolation across the chunk boundary. The virtual input
// stream is v[-1] = the last frame of the previous chunk, v[0..n-1] = this one,
// so g_pos carries fractional position between calls and a rate ratio of
// exactly 1.0 degenerates to a straight copy.
static size_t convert(const int16_t *src, size_t n, float **out) {
    double ratio = (double)g_in_rate / g_out_rate;
    size_t max_out = (size_t)((double)(n + 2) / ratio) + 2;
    if (max_out > g_scratch_frames) {
        float *p = realloc(g_scratch, max_out * g_out_ch * sizeof(float));
        if (!p) return 0;
        g_scratch = p;
        g_scratch_frames = max_out;
    }
    float *dst = g_scratch;
    unsigned ich = g_in_ch, och = g_out_ch;
    double pos = g_pos;
    size_t produced = 0;

    // The frame fetch folds channel mapping in: mono fans out, anything wider
    // than stereo keeps the first two channels. Unity's FMOD has only ever
    // asked for stereo, so the other paths are correctness, not tuning.
    // Clamped at BOTH ends. The upper clamp is not defensive tidiness: the loop
    // condition admits pos == n-1 (so that a 1.0 ratio degenerates to an exact
    // copy rather than dropping the last frame), and that reads v[n]. The
    // interpolation weight there is 0, so the value is discarded — but the read
    // is not, and it is two bytes past the guest's buffer.
    #define FETCH(idx, ch) \
        ((idx) < 0 ? g_prev[ch] \
                   : (float)src[(size_t)((idx) < (long)n ? (idx) : (long)n - 1) * ich \
                                + ((ch) < ich ? (ch) : 0)] * (1.0f / 32768.0f))

    while (pos < (double)n - 1.0 + 1e-9 && produced < max_out) {
        long i = (long)floor(pos);
        float f = (float)(pos - (double)i);
        for (unsigned c = 0; c < och; c++) {
            float a = FETCH(i, c), b = FETCH(i + 1, c);
            dst[produced * och + c] = a + (b - a) * f;
        }
        produced++;
        pos += ratio;
    }
    #undef FETCH

    if (n > 0) {
        for (unsigned c = 0; c < och; c++)
            g_prev[c] = (float)src[(n - 1) * ich + (c < ich ? c : 0)] * (1.0f / 32768.0f);
    }
    g_pos = pos - (double)n;
    if (g_pos < -1.0) g_pos = -1.0;
    *out = dst;
    return produced;
}

// The watchdog. A render callback that has stopped arriving while we believe we
// are running is the visionOS failure mode this file was written expecting:
// the OS reclaims the stream on a route change or a scene transition and the
// unit stays nominally started.
//
// **The heartbeat is the only honest signal.** visionOS silently stops calling
// the render callback when the immersive space is dismissed — a Digital Crown
// press, the app's window being closed — with no error, no interruption
// notification and a unit that still reports itself running. ALVR carries the
// same check for the same reason and against the same OS bug; there it lives in
// the receive loop, here it has a thread of its own (below).
//
// Consecutive restarts back off. A restart that cannot work — the app really is
// off screen, or the session really is inactive — would otherwise print twice a
// second for as long as that lasts, which buries the one line that says what
// actually happened.
static void watchdog(void) {
    if (!g_open || g_interrupted) return;
    uint64_t last = atomic_load_explicit(&g_last_render_ns, memory_order_relaxed);
    uint64_t idle = now_ns() - last;
    uint64_t limit = 500ull * 1000000ull;
    if (g_wd_streak > 3) limit = 3000ull * 1000000ull;    // backed off
    // The render callback's heartbeat is the ONLY honest signal that the device
    // is alive. g_running is our own bookkeeping, and on this OS AudioOutputUnitStop
    // does not reliably stop the callback firing — so a stale g_running=0 while the
    // callback keeps arriving must NOT tear the unit down. It did: with the callback
    // healthy (idle ~0.01 s) but g_running read as 0, this rebuilt the unit on every
    // pass — hundreds a run, ~2 ms apart from the producer's full-ring spin — and
    // each rebuild is a real gap. THAT was the "stutter", and no buffer could hide
    // it. So the decision is the heartbeat alone: the callback is alive, leave it be.
    if (idle <= limit) { g_wd_streak = 0; return; }
    // The callback has genuinely stopped. Only worth rebuilding if the guest still
    // wants sound; a stopped guest is just an idle device, not a fault to fight.
    if (!g_playing)    { g_wd_streak = 0; return; }
    if (g_wd_streak < 4 || (g_wd_streak % 20) == 0)
        fprintf(stderr, "  [au] no render callback for %.2f s — restarting%s\n",
                (double)idle / 1e9,
                g_wd_streak >= 4 ? " (still; further attempts are quiet)" : "");
    g_wd_streak++;
    kl_audio_restart();
}

// ...and the thread that runs it, because the producer cannot.
//
// `watchdog()` used to be reachable from exactly one place: the spin inside
// kl_audio_write that waits for the ring to drain. That covers the case the
// file was written for — the render callback dies while the guest keeps
// producing — and misses every other one. Two that matter here:
//
//   * The guest stops producing too. Steam Link's audio is a pull path
//     (kl_aaudio.c calls the guest's data callback), and a guest whose own
//     clock has stalled writes nothing, so nothing checks.
//   * An interruption that never ends. `kl_audio_interrupted(1)` latches
//     g_interrupted, and the write loop breaks out of the spin *before* the
//     watchdog on that flag — so a `.began` with no matching `.ended` (which is
//     a documented hazard on this OS family, and is what a scene transition
//     looks like when the notification is dropped) is silence for the rest of
//     the run with nothing left running to notice.
//
// A detached thread at 4 Hz costs nothing and depends on neither side.
static void *watchdog_thread(void *unused) {
    (void)unused;
    pthread_setname_np("klepton-audio-watchdog");
    for (;;) {
        nap_ms(250);
        if (!g_open) continue;
        // The stuck-interruption escape. The OS deactivated the session on the
        // way in and is supposed to tell us on the way out; when it does not,
        // the only thing that distinguishes "still interrupted" from "the
        // notification was lost" is trying. Bounded, so a real interruption (a
        // call, another app taking the route) is not fought over.
        if (g_interrupted && g_playing) {
            uint64_t since = now_ns() - g_interrupt_ns;
            if (since > (uint64_t)g_interrupt_max_ms * 1000000ull) {
                fprintf(stderr, "  [au] interrupted for %.1f s with no resume — "
                                "assuming the notification was lost\n",
                        (double)since / 1e9);
                g_interrupted = 0;
                g_interrupt_ns = now_ns();
                kl_audio_restart();
                // If this restart lands on a session that is STILL reactivating
                // it plays silence and the normal watchdog below would rebuild it
                // again every 500 ms — the jump storm. Pre-load the backoff so the
                // next attempt is seconds away, not milliseconds: one more try,
                // spaced, by which point the session is certainly back.
                g_wd_streak = 4;
            }
            continue;
        }
        watchdog();
    }
    return NULL;
}

static void watchdog_start(void) {
    static int started;
    if (started) return;
    if (!kl_env_on("KL_AUDIO_WATCHDOG", 1)) {
        fprintf(stderr, "  [au] KL_AUDIO_WATCHDOG=0 — no independent watchdog\n");
        started = 1;
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, watchdog_thread, NULL) != 0) {
        fprintf(stderr, "  [au] could not start the audio watchdog thread\n");
        return;
    }
    pthread_detach(t);
    started = 1;
}

// The app is back on screen. Whatever this file believes about its own state,
// rebuild — and this is deliberately NOT conditional on the heartbeat.
//
// The heartbeat costs up to a second to notice, and it notices by finding
// silence that has already been heard. A compositor coming out of `.paused`
// knows the transition exactly, at the moment it happens, so the gap it leaves
// is the rebuild itself. The two overlap on purpose: this is precise and can be
// missed, the heartbeat is late and cannot.
void kl_audio_resume(void) {
    if (!g_open) return;
    int was = g_interrupted;
    g_interrupted = 0;
    g_interrupt_ns = now_ns();
    g_wd_streak = 0;
    fprintf(stderr, "  [au] resume%s\n", was ? " (was interrupted)" : "");
    kl_audio_restart();
}

size_t kl_audio_write(const void *pcm, size_t bytes) {
    return kl_audio_write_src(NULL, pcm, bytes);
}

size_t kl_audio_write_src(const void *src, const void *pcm, size_t bytes) {
    if (!g_open || !pcm || !bytes) return 0;
    unsigned in_frame = g_in_ch * 2;              // 16-bit, checked at open
    size_t n = bytes / in_frame;
    if (!n) return 0;

    int peak = 0;
    const int16_t *s16 = (const int16_t *)pcm;
    for (size_t i = 0; i < n * g_in_ch; i++) {
        int v = s16[i] < 0 ? -s16[i] : s16[i];
        if (v > peak) peak = v;
    }
    if (peak > g_peak) g_peak = peak;
    if (!peak) g_silent_buffers++;

    // The convert()/scratch state and the mix cursors are shared, so the whole
    // body is serialised: two feeder threads (Steam Link's two output streams)
    // reach here at once. The reader never takes this lock.
    pthread_mutex_lock(&g_wlock);

    float *conv = NULL;
    size_t frames = convert((const int16_t *)pcm, n, &conv);
    if (!frames) { pthread_mutex_unlock(&g_wlock); return 0; }
    g_writes++;

    // The target has to be able to hold at least two of these, or the wait
    // below can never be satisfied and every write times out.
    if (g_target < frames * 2) {
        g_target = frames * 2;
        if (g_target > g_cap - 1) g_target = g_cap - 1;
    }

    int slot = src_slot(src);
    uint64_t r0 = atomic_load_explicit(&g_r, memory_order_acquire);
    uint64_t pos = g_src_pos[slot];
    // (Re)sync a new or stalled source to the latency target ahead of the reader,
    // so its samples mix with the others in step — not in the past (already
    // played as silence) nor so far ahead the ring wraps under it.
    if (pos < r0 || pos > r0 + g_cap) {
        uint64_t back = g_target > frames ? g_target - frames : 0;
        pos = r0 + back;
    }

    // Block until this source is no more than the latency target ahead of the
    // reader — THIS is the clock, exactly as the single-source path was, and it
    // paces both streams to the device rate.
    uint64_t deadline = now_ns() + 2000ull * 1000000ull;
    size_t written = 0;
    while (written < frames) {
        uint64_t r = atomic_load_explicit(&g_r, memory_order_acquire);
        uint64_t here = pos + written;
        if (here < r) here = r;                       // never write in the past
        uint64_t ahead = here - r;
        size_t room = ahead < g_target ? (size_t)(g_target - ahead) : 0;
        if (ahead < g_cap && g_cap - (size_t)ahead < room) room = g_cap - (size_t)ahead;
        size_t take = frames - written;
        if (take > room) take = room;

        if (take) {
            for (size_t k = 0; k < take; k++) {
                size_t off = (size_t)((here + k) % g_cap);
                const float *sp = conv + (written + k) * g_out_ch;
                float *dp = g_ring + off * g_out_ch;
                for (unsigned c = 0; c < g_out_ch; c++) dp[c] += sp[c];   // MIX
            }
            written += take;
            uint64_t nf = pos + written;
            if (nf < r) nf = r;
            uint64_t curw = atomic_load_explicit(&g_w, memory_order_relaxed);
            if (nf > curw) atomic_store_explicit(&g_w, nf, memory_order_release);
            continue;
        }

        // Full. A stop / interruption / lost device must break the loop, or the
        // feeder wedges holding nothing and the guest's mixer stalls behind it.
        if (!g_playing || g_interrupted) break;
        watchdog();
        if (now_ns() > deadline) { g_short_writes++; break; }
        nap_ms(2);
    }
    g_src_pos[slot] = pos + written < r0 ? r0 : pos + written;

    dump_write(conv, written);

    if (g_trace && (g_writes % 200) == 1) {
        size_t fill = atomic_load(&g_w) - atomic_load(&g_r);
        fprintf(stderr, "  [au] write #%lu src%d: %zu in -> %zu out, fill %zu/%zu "
                        "(%.1f ms), %d src(s), underruns %u\n",
                g_writes, slot, n, frames, fill, g_target,
                1000.0 * (double)fill / g_out_rate, g_src_n,
                atomic_load(&g_underruns));
    }
    pthread_mutex_unlock(&g_wlock);
    return written == frames ? bytes : written * in_frame;
}

unsigned kl_audio_underruns(void) { return atomic_load(&g_underruns); }

void kl_audio_report(FILE *f) {
    if (!g_writes && !g_open) return;
    dump_close();   // the report is the last thing a clean run does
    uint64_t played = atomic_load(&g_frames_played);
    fprintf(f, "  CoreAudio: %.2f s played, %u underruns (%.3f s of silence filled), "
               "%lu writes (%lu short), %lu restarts\n",
            g_out_rate > 0 ? (double)played / g_out_rate : 0.0,
            atomic_load(&g_underruns),
            g_out_rate > 0 ? (double)atomic_load(&g_frames_silent) / g_out_rate : 0.0,
            g_writes, g_short_writes, g_restarts);
    fprintf(f, "  guest PCM: peak %d/32767 (%.1f dBFS), %lu of %lu buffers all-zero — %s\n",
            g_peak, g_peak ? 20.0 * log10((double)g_peak / 32767.0) : -1e9,
            g_silent_buffers, g_writes,
            g_peak ? "the guest produced audio" : "THE GUEST PRODUCED ONLY SILENCE");
}
