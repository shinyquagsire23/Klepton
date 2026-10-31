// See kl_audio.h. The producer/consumer split, in one paragraph:
//
//   feeder thread (kl_opensl.c)          CoreAudio render thread
//   ----------------------------          -----------------------
//   pop guest buffer                      memcpy out of the ring
//   guest format -> float32, N ch -> out  advance the read cursor
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
#include "kl_key.h"
#include "kl_audio.h"
#include "guest/kl_driver.h"

#define KL_AUDIO_MAX_CH 2

// ---- state ----
static AudioUnit    g_unit;
static int          g_open;           // a unit exists and is initialised
static int          g_running;        // ...and AudioOutputUnitStart succeeded
static int          g_playing;        // the guest wants sound
static int          g_interrupted;    // the OS took the stream
static int          g_silenced;       // the scene took it — deliberate, see below
static uint64_t     g_interrupt_ns;   // ...when, so a lost `.ended` is escapable
static unsigned     g_interrupt_max_ms = 1500;
// The recovery machinery lives at the bottom of the file, beside the producer
// it used to be part of; kl_audio_open and kl_audio_interrupted are above it.
static unsigned     g_wd_streak;      // consecutive restarts, for the backoff
static void         watchdog_start(void);
// ---- the locks, and the ONE order they are taken in ----
//
//   g_prod  ->  g_lock  ->  g_wlock          (g_write: only ever TRY-locked)
//
//   g_prod   the source table and the arbitration (below). Held while the
//            arbitration opens the device for a new owner (sink_claim ->
//            sink_open), so it comes first.
//   g_lock   the unit's lifecycle: create/start/stop/destroy.
//   g_wlock  everything a writer touches: the ring (g_ring, g_cap, g_target,
//            the write cursor), the owner's resampler and scratch (g_pos,
//            g_prev, g_scratch, g_inbuf) and the per-source slot table. So
//            every place that frees, replaces or resets those — sink_open,
//            kl_audio_close, kl_audio_flush, kl_audio_restart — takes g_wlock
//            too, inside g_lock.
//   g_write  the single-producer body; try-locked by a writer holding no other
//            lock, and g_wlock is taken inside it. Never held while taking
//            g_prod or g_lock.
//
// Nothing takes g_lock while holding g_wlock. That is why a writer that finds
// the render callback dead does not call the watchdog from inside its wait (the
// watchdog's restart takes g_lock): it breaks out, drops its locks, and THEN
// runs it. The render callback takes none of them; the ring is only freed or
// replaced after unit_destroy, which waits for the callback to return.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   // unit lifecycle only

static unsigned g_in_rate, g_in_ch, g_in_bits;   // the guest's format
static kl_audio_fmt g_in_fmt = KL_AUDIO_S16;     // ...and its sample encoding
static float   *g_inbuf;                         // guest samples widened to float
static size_t   g_inbuf_samples;
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
//
// Which sources get here at all is the arbitration's call (below, "the source
// table"): normally only the one that owns the output, and every source only
// when KL_AUDIO_MIX_SOURCES is on (Steam Link by default). Mixed sources keep
// their own resampler position here, because each one is widened from its own
// format and resampled from its own rate.
static _Atomic uint64_t g_frames_played, g_frames_silent;
#define KL_AU_MAX_SRC 4
static const void      *g_src_key[KL_AU_MAX_SRC];
static uint64_t         g_src_pos[KL_AU_MAX_SRC];   // next write frame, per source
// For the periodic verdict while mixing: frames each source handed over and how
// often it fell behind the reader and was resynced. Two sources that share the
// device must each deliver real time; the total write count cannot say which
// one does not (Steam Link, community fix #49).
static uint64_t         g_src_frames[KL_AU_MAX_SRC];
static uint64_t         g_src_since[KL_AU_MAX_SRC];  // g_frames_played when it came
static unsigned long    g_src_resyncs[KL_AU_MAX_SRC];
static double           g_src_rpos[KL_AU_MAX_SRC];   // resampler position, mixed sources
static float            g_src_prev[KL_AU_MAX_SRC][KL_AUDIO_MAX_CH];
static int              g_src_n;
static pthread_mutex_t  g_wlock = PTHREAD_MUTEX_INITIALIZER;
// Bumped (under g_wlock) whenever the ring is freed or replaced, so a writer
// that let go of g_wlock for a wait (KL_AUDIO_MIX_UNLOCK) can tell that what it
// was writing into is gone.
static uint64_t         g_ring_gen;

// Slot for a producer key (NULL = the single-source/OpenSL path -> slot 0).
// Caller holds g_wlock. Overflow folds into slot 0 rather than dropping audio.
static int src_slot(const void *key) {
    if (!key) return 0;
    for (int i = 0; i < g_src_n; i++) if (g_src_key[i] == key) return i;
    if (g_src_n < KL_AU_MAX_SRC) {
        int i = g_src_n++;
        g_src_key[i] = key; g_src_pos[i] = 0;
        g_src_frames[i] = 0; g_src_resyncs[i] = 0;
        g_src_since[i] = atomic_load(&g_frames_played);
        g_src_rpos[i] = 0.0;
        memset(g_src_prev[i], 0, sizeof g_src_prev[i]);
        return i;
    }
    return 0;
}

// A producer that is gone. Without this a closed stream's key stays in the
// table for the life of the process: a guest that opens and closes its stream
// repeatedly fills all four slots, and from the second one on every write takes
// the MIXING path against cursors nobody advances.
static void src_slot_release(const void *key) {
    if (!key) return;
    pthread_mutex_lock(&g_wlock);
    for (int i = 0; i < g_src_n; i++)
        if (g_src_key[i] == key) {
            for (int k = i + 1; k < g_src_n; k++) {
                g_src_key[k - 1]     = g_src_key[k];
                g_src_pos[k - 1]     = g_src_pos[k];
                g_src_frames[k - 1]  = g_src_frames[k];
                g_src_since[k - 1]   = g_src_since[k];
                g_src_resyncs[k - 1] = g_src_resyncs[k];
                g_src_rpos[k - 1]    = g_src_rpos[k];
                memcpy(g_src_prev[k - 1], g_src_prev[k], sizeof g_src_prev[k]);
            }
            g_src_n--;
            break;
        }
    pthread_mutex_unlock(&g_wlock);
}

// A driver-set floor for the fill target, in ms (0 = none). See kl_audio_open.
static unsigned         g_latency_override_ms;
void kl_audio_set_latency_ms(unsigned ms) { g_latency_override_ms = ms; }

// Resampler state, producer-side only.
static double g_pos;                     // position in the input stream
static float  g_prev[KL_AUDIO_MAX_CH];   // last input frame of the last chunk
static float *g_scratch;                 // converted+resampled staging
static size_t g_scratch_frames;          // ...its size in samples (see resample)

// Counters, for the report and for the watchdog.
static _Atomic uint64_t g_last_render_ns;
static _Atomic uint64_t g_last_render_frames;   // what that callback asked for
static _Atomic uint32_t g_underruns;
static unsigned long    g_writes, g_short_writes, g_restarts;
static int              g_trace;
// The peak sample the guest ever handed us. A buffer count says the mixer ran;
// this says the mixer produced audio — the same distinction `make check` draws
// for the opus roundtrip, and the one that would have caught "3,238 buffers
// consumed, nothing heard" years earlier.
static int              g_peak;
static unsigned long    g_silent_buffers;

// ---- microphone capture state ----
//
// A SECOND, independent RemoteIO unit — capture only — so nothing here can
// perturb the output unit above. It is armed by the KleptonMic toggle
// (g_mic_enabled) and only actually built when a guest opens an input stream
// (kl_audio_mic_open), which is what keeps the hardware — and the visionOS
// permission prompt — untouched until a person opts in AND a guest asks.
//
// The ring is int16 interleaved at the device capture rate, and it is
// single-producer/single-consumer exactly like the output ring, but the roles
// are swapped: the real-time INPUT callback is the producer, and the guest's
// reader thread (kl_aaudio.c) is the consumer. So the callback owns g_mic_w and
// the reader owns g_mic_r, and neither takes a lock. Resampling and format /
// channel conversion to the guest's requested shape live on the CONSUMER side
// (kl_aaudio.c), for the same reason the output path keeps them off the render
// thread: kl_audio_mic_read_i16 hands back exactly what the hardware gave, at
// kl_audio_mic_rate() / kl_audio_mic_channels().
static AudioUnit        g_cap_unit;
static int              g_mic_enabled;    // the KleptonMic toggle
static int              g_mic_open;       // a capture unit exists and is running
static unsigned         g_mic_rate;       // the CLIENT rate the ring holds (== guest rate when the converter took it)
static unsigned         g_mic_dev_rate;   // the hardware's own capture rate, for the log / fallback
static unsigned         g_mic_ch = 1;     // capture channels (1 or 2)
static pthread_mutex_t  g_mic_lock = PTHREAD_MUTEX_INITIALIZER;   // unit lifecycle

static int16_t         *g_mic_ring;
static size_t           g_mic_cap;        // frames
static _Atomic size_t   g_mic_w, g_mic_r;

// The input callback pulls the hardware's frames into this fixed scratch and
// then copies them into the ring — a real-time callback may not allocate, so the
// AudioBufferList and its backing buffer are built once at open. g_cap_abl has
// the single interleaved buffer AudioBufferList's flexible mBuffers[1] provides.
static AudioBufferList  g_cap_abl;
static int16_t         *g_cap_scratch;
static size_t           g_cap_scratch_frames;

// Counters for the report. Producer-side (the callback) writes the capture/drop
// pair with relaxed atomics — no log or malloc on that thread — and g_mic_peak
// is the capture twin of g_peak: "did the microphone actually hear anything".
static _Atomic uint64_t g_mic_frames_cap, g_mic_frames_drop, g_mic_frames_read;
static _Atomic int      g_mic_peak;

// ...and the ones the mic heartbeat (mic_heartbeat) prints per interval. A
// report of "peak 32767" since the start cannot tell one loud cough from a mic
// that clips all the time, so the heartbeat's peak is reset on every line, and
// a buffer that touched full scale (±32767: the int16 client format has nowhere
// to put more, so an AGC that overshoots is a hard clip) is counted.
static _Atomic int      g_mic_peak_iv;         // since the last heartbeat line
static _Atomic uint64_t g_mic_bufs, g_mic_bufs_clip;
static _Atomic uint32_t g_mic_cb_frames;       // the last callback's size, as asked
static _Atomic uint32_t g_mic_cb_min, g_mic_cb_max;   // ...and its range since the last line
static int              g_mic_agc = -1;        // AGC as read back from VPIO: -1 unknown / not VPIO

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// ---- the source table: which guest backend owns the one output ----
//
// See the long note in kl_audio.h. Everything below the arbitration point in
// this file assumes a single producer; the table is what makes that true again
// when the guest runs two.
#define KL_AUDIO_MAX_SRC 4
typedef struct {
    const void   *tag;
    const char   *name;
    unsigned      rate, ch;
    kl_audio_fmt  fmt;
    unsigned long writes, silent;
    int           peak;
    int           playing;         // between its play and its pause
    uint64_t      lit_ns;          // last buffer that was not all zeroes
} kl_src;
static kl_src       g_src[KL_AUDIO_MAX_SRC];
static int          g_nsrc;
static const void  *g_owner;
static const char  *g_forced;      // KL_AUDIO_SOURCE
static const char  *g_prefer;      // KL_AUDIO_PREFER
static int          g_mix_all;     // KL_AUDIO_MIX_SOURCES — every source mixed
static int          g_mix_unlock;  // KL_AUDIO_MIX_UNLOCK
static int          g_smooth;      // KL_AUDIO_SMOOTH
static unsigned     g_steal_ms;    // KL_AUDIO_STEAL_MS
// Producer-side reporters, appended to the live report. See kl_audio_reporter.
static void       (*g_report[KL_AUDIO_MAX_SRC])(FILE *);
static int          g_nreport;
static pthread_mutex_t g_prod  = PTHREAD_MUTEX_INITIALIZER;  // the table above
static pthread_mutex_t g_write = PTHREAD_MUTEX_INITIALIZER;  // the producer body

static kl_src *src_find(const void *tag) {
    if (!tag) return NULL;
    for (int i = 0; i < g_nsrc; i++) if (g_src[i].tag == tag) return &g_src[i];
    return NULL;
}

static kl_src *src_add(const void *tag, const char *name,
                       unsigned rate, unsigned ch, kl_audio_fmt fmt) {
    kl_src *s = src_find(tag);
    if (s) { s->rate = rate; s->ch = ch; s->fmt = fmt; return s; }
    if (!tag || g_nsrc >= KL_AUDIO_MAX_SRC) return NULL;
    s = &g_src[g_nsrc++];
    s->tag = tag; s->name = name ? name : "?";
    s->rate = rate; s->ch = ch; s->fmt = fmt;
    s->lit_ns = now_ns();
    if (g_nsrc > 1 && g_mix_all)
        fprintf(stderr, "  [au] a SECOND guest audio stream appeared: '%s' "
                        "%u Hz/%u ch/%s (the first was '%s' %u Hz/%u ch/%s). "
                        "KL_AUDIO_MIX_SOURCES is on, so both are mixed, each "
                        "from its own format and rate.\n",
                s->name, s->rate, s->ch, kl_audio_fmt_name(s->fmt),
                g_src[0].name, g_src[0].rate, g_src[0].ch, kl_audio_fmt_name(g_src[0].fmt));
    else if (g_nsrc > 1)
        fprintf(stderr, "  [au] a SECOND guest audio backend appeared: '%s' "
                        "%u Hz/%u ch (the first was '%s' %u Hz/%u ch). One of "
                        "them will own the output; the other is paced and "
                        "dropped — KL_AUDIO_SOURCE=<name> forces the choice, "
                        "KL_AUDIO_MIX_SOURCES=1 mixes both.\n",
                s->name, s->rate, s->ch, g_src[0].name, g_src[0].rate, g_src[0].ch);
    return s;
}

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
//
// It is BOUNDED and SELF-CLOSING, and the writer lives in kl_wav_* so the
// producer side can tee too. See kl_audio.h: on device the exit path is
// unreachable, so a capture that only patches its header at close is a capture
// nobody can play. A cap the file closes itself at is the whole difference.
struct kl_wav {
    FILE    *f;
    unsigned rate, ch;
    size_t   frames, max_frames;
    char     path[1024];
};

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

double kl_audio_dump_seconds(void) {
    int n = kl_env_int("KL_AUDIO_DUMP_S", 20);
    if (n < 0) n = 0;
    if (n > 300) n = 300;
    return (double)n;
}

kl_wav *kl_wav_open(const char *path, unsigned rate, unsigned ch, double max_s) {
    if (!path || !*path || !rate || !ch || !(max_s > 0.0)) return NULL;
    kl_wav *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->f = fopen(path, "wb");
    if (!w->f) {
        fprintf(stderr, "  [au] cannot write %s\n", path);
        free(w);
        return NULL;
    }
    w->rate = rate; w->ch = ch;
    w->max_frames = (size_t)(max_s * (double)rate);
    snprintf(w->path, sizeof w->path, "%s", path);
    // Sizes are patched at close; a run that dies without closing leaves a
    // header claiming zero, which most players read as an empty file. Better
    // than a header claiming a length the data does not have.
    fwrite("RIFF", 1, 4, w->f); put32(w->f, 0); fwrite("WAVEfmt ", 1, 8, w->f);
    put32(w->f, 16); put16(w->f, 1); put16(w->f, (uint16_t)ch);
    put32(w->f, rate);
    put32(w->f, rate * ch * 2u);
    put16(w->f, (uint16_t)(ch * 2u)); put16(w->f, 16);
    fwrite("data", 1, 4, w->f); put32(w->f, 0);
    // The header line the analysis is read against. Spelled out in full —
    // "48000 Hz, 2 ch, s16le interleaved" is the thing a raw dump would need a
    // note beside it for, and the note is what goes missing.
    fprintf(stderr, "  [au] CAPTURE -> %s (WAV, %u Hz, %u ch, signed 16-bit "
                    "little-endian, interleaved, %.0f s max = %.1f MB)\n",
            path, rate, ch, max_s,
            (double)w->max_frames * ch * 2.0 / (1024.0 * 1024.0));
    return w;
}

static void wav_finish(kl_wav *w) {
    if (!w || !w->f) return;
    uint32_t bytes = (uint32_t)(w->frames * w->ch * 2u);
    fseek(w->f, 4, SEEK_SET);  put32(w->f, 36 + bytes);
    fseek(w->f, 40, SEEK_SET); put32(w->f, bytes);
    fclose(w->f);
    w->f = NULL;
    fprintf(stderr, "  [au] CAPTURE closed: %s — %.2f s (%zu frames, %u bytes)\n",
            w->path, w->rate ? (double)w->frames / w->rate : 0.0,
            w->frames, bytes);
}

void kl_wav_write_s16(kl_wav *w, const int16_t *pcm, size_t frames) {
    if (!w || !w->f || !pcm || !frames) return;
    if (w->frames + frames > w->max_frames) frames = w->max_frames - w->frames;
    if (frames) {
        fwrite(pcm, w->ch * 2u, frames, w->f);
        w->frames += frames;
    }
    if (w->frames >= w->max_frames) wav_finish(w);
}

void kl_wav_write_f32(kl_wav *w, const float *pcm, size_t frames) {
    if (!w || !w->f || !pcm || !frames) return;
    if (w->frames + frames > w->max_frames) frames = w->max_frames - w->frames;
    for (size_t i = 0; i < frames * w->ch; i++) {
        float v = pcm[i];
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        put16(w->f, (uint16_t)(int16_t)lrintf(v * 32767.0f));
    }
    w->frames += frames;
    if (w->frames >= w->max_frames) wav_finish(w);
}

void kl_wav_close(kl_wav *w) {
    if (!w) return;
    wav_finish(w);
    free(w);
}

// The sink's own capture. Armed by the first buffer that carries signal from
// the OWNER — arming on the first write instead would spend the whole cap on
// the boot window, which on Tetris Effect is 4,954 silent buffers and 25 s.
static kl_wav *g_dump;
static int     g_dump_armed;

static void dump_open(void) {
    const char *path = kl_env_str("KL_AUDIO_DUMP", NULL);
    if (!path || !*path) return;
    g_dump = kl_wav_open(path, (unsigned)g_out_rate, g_out_ch,
                         kl_audio_dump_seconds());
    if (g_dump)
        fprintf(stderr, "  [au]   ...that is the DEVICE end of the path: "
                        "post-resample, post-arbitration, exactly the frames "
                        "the render callback hands CoreAudio\n");
}

static void dump_write(const float *frames, size_t n) {
    kl_wav_write_f32(g_dump, frames, n);
}

static void dump_close(void) {
    kl_wav_close(g_dump);
    g_dump = NULL;
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
    atomic_store_explicit(&g_last_render_frames, nframes, memory_order_relaxed);

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

// ---- sample formats ----
//
// Everything downstream of here is float32. Widening at the door rather than
// inside the resampler keeps the interpolation loop branch-free and means a new
// guest encoding is one case in one switch, not a second copy of the resampler.
unsigned kl_audio_fmt_bytes(kl_audio_fmt fmt) {
    switch (fmt) {
    case KL_AUDIO_U8:  return 1;
    case KL_AUDIO_S24: return 3;
    case KL_AUDIO_S32: return 4;
    case KL_AUDIO_F32: return 4;
    case KL_AUDIO_S16: default: return 2;
    }
}

int kl_audio_fmt_from_bits(unsigned bits, kl_audio_fmt *out) {
    switch (bits) {
    case 8:  *out = KL_AUDIO_U8;  return 0;
    case 16: *out = KL_AUDIO_S16; return 0;
    case 24: *out = KL_AUDIO_S24; return 0;
    case 32: *out = KL_AUDIO_S32; return 0;
    default: return -1;
    }
}

const char *kl_audio_fmt_name(kl_audio_fmt fmt) {
    switch (fmt) {
    case KL_AUDIO_U8:  return "u8";
    case KL_AUDIO_S24: return "s24";
    case KL_AUDIO_S32: return "s32";
    case KL_AUDIO_F32: return "f32";
    case KL_AUDIO_S16: default: return "s16";
    }
}

void kl_audio_to_float(kl_audio_fmt fmt, const void *src, float *dst, size_t n) {
    if (!src || !dst) return;
    switch (fmt) {
    case KL_AUDIO_F32: {
        // Nominal [-1, 1], but a spatialiser's bus can overshoot and an int16
        // sink would have clipped it. Clamp so it clips the same way rather
        // than wrapping into the opposite polarity further down.
        const float *s = (const float *)src;
        for (size_t i = 0; i < n; i++) {
            float v = s[i];
            if (!(v == v)) v = 0.0f;              // NaN -> silence, not noise
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            dst[i] = v;
        }
        break;
    }
    case KL_AUDIO_S32: {
        const int32_t *s = (const int32_t *)src;
        for (size_t i = 0; i < n; i++) dst[i] = (float)s[i] * (1.0f / 2147483648.0f);
        break;
    }
    case KL_AUDIO_S24: {
        // Packed, three bytes little-endian. Sign-extend through the top byte.
        const uint8_t *s = (const uint8_t *)src;
        for (size_t i = 0; i < n; i++) {
            int32_t v = (int32_t)((uint32_t)s[i * 3] << 8 |
                                  (uint32_t)s[i * 3 + 1] << 16 |
                                  (uint32_t)s[i * 3 + 2] << 24);
            dst[i] = (float)v * (1.0f / 2147483648.0f);
        }
        break;
    }
    case KL_AUDIO_U8: {
        const uint8_t *s = (const uint8_t *)src;
        for (size_t i = 0; i < n; i++) dst[i] = ((float)s[i] - 128.0f) * (1.0f / 128.0f);
        break;
    }
    case KL_AUDIO_S16:
    default: {
        const int16_t *s = (const int16_t *)src;
        for (size_t i = 0; i < n; i++) dst[i] = (float)s[i] * (1.0f / 32768.0f);
        break;
    }
    }
}

// ---- open / close ----
static int sink_open(unsigned rate, unsigned channels, kl_audio_fmt fmt) {
    unsigned bits = kl_audio_fmt_bytes(fmt) * 8;
    if (!kl_env_on("KL_AUDIO", 1)) {
        static int said;
        if (!said++) fprintf(stderr, "  [au] KL_AUDIO=0 — no device, the feeder keeps its own clock\n");
        return -1;
    }
    if (channels < 1 || channels > 8 || rate < 8000 || rate > 192000) {
        fprintf(stderr, "  [au] refusing guest format %u Hz / %u ch / %s\n",
                rate, channels, kl_audio_fmt_name(fmt));
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    if (g_open && rate == g_in_rate && channels == g_in_ch && fmt == g_in_fmt) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    // Everything below replaces what a writer reads. A writer waiting for room
    // holds g_wlock until its buffer is in, which the still-running unit lets
    // it finish; destroying the unit first would leave it to time out.
    pthread_mutex_lock(&g_wlock);
    if (g_open) unit_destroy();
    g_open = 0;

    g_in_rate = rate; g_in_ch = channels; g_in_bits = bits; g_in_fmt = fmt;
    g_out_ch = channels == 1 ? 1 : 2;
    g_trace = kl_env_on("KL_AUDIO_TRACE", 0);

    if (unit_create() != 0) {
        pthread_mutex_unlock(&g_wlock);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

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
    free(g_inbuf);
    g_inbuf = NULL;
    g_inbuf_samples = 0;
    g_ring_gen++;
    // A different format is a different ring: every source cursor described
    // the old one.
    g_src_n = 0;
    if (!g_ring) {
        unit_destroy();
        pthread_mutex_unlock(&g_wlock);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

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
    pthread_mutex_unlock(&g_wlock);
    pthread_mutex_unlock(&g_lock);
    watchdog_start();

    fprintf(stderr, "  [au] CoreAudio out: guest %u Hz/%u ch/%s -> device %.0f Hz/%u ch, "
                    "%u ms target%s\n",
            rate, channels, kl_audio_fmt_name(fmt), g_out_rate, g_out_ch, lat_ms,
            fabs(g_out_rate - (double)rate) > 1.0 ? " (resampling)" : "");
    return 0;
}

// The arbitration, and the six source-scoped entry points that use it.
//
// Two rules, and they are the whole design:
//   * SILENCE NEVER CLAIMS. A backend that has only ever produced zeroes has
//     shown nothing, and letting it claim at boot is a coin flip decided by
//     whichever mixer spun up first — which on Tetris Effect is UE4's, the one
//     that is silent all run because everything audible goes through Wwise.
//   * The claim is REVOCABLE. If the owner goes quiet for KL_AUDIO_STEAL_MS
//     while a challenger is producing sound, the challenger takes it. That is
//     what makes a wrong first guess cost two seconds instead of the run.
//
// Two titles need more than the rules, and each has a knob with a per-title
// default (the knob always wins):
//   * KL_AUDIO_MIX_SOURCES — every source reaches the device, MIXED. Steam
//     Link keeps two AAudio streams open during a stream, the 2D shell's SDL3
//     one (float, 960 frames) and the VR client's own (int16, 256), and both
//     carry sound: one owner would throw half of it away. On by default for
//     steamlink-vr (community fix #49).
//   * KL_AUDIO_PREFER=<backend> — while a source of that backend is open it
//     owns the output from its first buffer, heard or not, and every other one
//     is paced and dropped. Pistol Whip runs Unity's own OpenSL player, which
//     is near-silent, beside Wwise's AAudio stream, which carries the whole
//     game: "the first one heard" could be the wrong one for as long as Unity's
//     player is not exactly zero. `aaudio` by default for x2104963472963790
//     (community fixes #26/#27: AAudio owns the device exclusively).
static int target_is(const char *t, const char *name) { return t && strcmp(t, name) == 0; }

static void sink_env(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *t = kl_driver_target_name();
    g_forced   = kl_env_str("KL_AUDIO_SOURCE", NULL);
    if (g_forced && !*g_forced) g_forced = NULL;
    g_prefer   = kl_env_str("KL_AUDIO_PREFER",
                            target_is(t, "x2104963472963790") ? "aaudio" : NULL);
    if (g_prefer && !*g_prefer) g_prefer = NULL;
    // Roboquest VR, Meta Store build (x9269393463181825, UE 5.6): FMOD's OpenSL
    // player (48 kHz) and Unreal's own mixer (44.1 kHz) both carry the game's
    // sound; with one owner, whichever was dropped went missing (on device,
    // 2026-10-23: "some audio effects might be missing").
    g_mix_all  = kl_env_on("KL_AUDIO_MIX_SOURCES", target_is(t, "steamlink-vr") ||
                                                    target_is(t, "x9269393463181825"));
    // Whether a MIXING writer lets go of g_wlock while it waits for room.
    // Holding it across the 2 ms naps serialises the sources: each one's wait
    // for ITS cursor blocks every other source's write for a whole buffer
    // period. Steam Link's pair managed ~200 writes a second that way where real
    // time is ~237 — the sound cutting out every few seconds (community fix
    // #49); Pistol Whip's Unity + Wwise pair delivered ~25% of real time when
    // they were still mixed (fix 10). Every ring write still happens under the
    // lock; only the nap is outside it. Only read while mixing.
    g_mix_unlock = kl_env_on("KL_AUDIO_MIX_UNLOCK", target_is(t, "steamlink-vr") ||
                                                    target_is(t, "x2104963472963790") ||
                                                    target_is(t, "x9269393463181825"));
    // See pacing_reader. LES MILLS BODYCOMBAT.
    g_smooth   = kl_env_on("KL_AUDIO_SMOOTH", target_is(t, "x4015163475201433"));
    g_steal_ms = kl_env_int("KL_AUDIO_STEAL_MS", 2000);
    if (g_mix_all)
        fprintf(stderr, "  [au] KL_AUDIO_MIX_SOURCES=1 — every guest audio stream "
                        "is mixed into the output, each converted from its own "
                        "format and rate%s\n",
                g_mix_unlock ? "; a writer waits for room outside the mix lock "
                               "(KL_AUDIO_MIX_UNLOCK)" : "");
    if (g_forced)
        fprintf(stderr, "  [au] KL_AUDIO_SOURCE=%s — only that backend reaches "
                        "the device\n", g_forced);
    else if (g_prefer && !g_mix_all)
        fprintf(stderr, "  [au] KL_AUDIO_PREFER=%s — while that backend has a "
                        "stream open it owns the output\n", g_prefer);
}

// Hand the output to `s`. Rebuilds the unit if its format is not the one the
// device was opened against, which is also what stops a second backend's
// open() from doing that rebuild speculatively.
static void sink_claim(kl_src *s) {
    const char *was = NULL;
    if (g_owner) { kl_src *o = src_find(g_owner); was = o ? o->name : "?"; }
    g_owner = s->tag;
    fprintf(stderr, "  [au] '%s' (%u Hz/%u ch) now owns the output%s%s\n",
            s->name, s->rate, s->ch,
            was ? ", taken from '" : "", was ? was : "");
    // fmt is part of the identity too: two owners at the same rate/ch but
    // different encodings (an s16 AAudio stream handing off to an f32 OpenSL
    // one) must reopen, or the widening below reads the new bytes as the old
    // format — the float-as-int16 static, reintroduced through the handoff.
    // A device that is not open at all is opened: a flush of nothing would
    // leave the new owner silent for the rest of the run.
    if (!g_open || s->rate != g_in_rate || s->ch != g_in_ch || s->fmt != g_in_fmt)
        sink_open(s->rate, s->ch, s->fmt);
    else
        kl_audio_flush();
    // Its play() may have come while it was not the owner, and was not passed on.
    if (s->playing) kl_audio_play();
}

static int is_preferred(const kl_src *s) {
    return g_prefer && s && strcmp(s->name, g_prefer) == 0;
}

// 1 = this buffer goes to the device, 0 = pace it and drop it.
static int arbitrate(kl_src *s, int peak) {
    sink_env();
    if (g_mix_all) return 1;
    if (!s) return g_owner == NULL;         // an untagged writer only runs unopposed
    if (g_forced) {
        if (strcmp(s->name, g_forced) != 0) return 0;
        if (g_owner != s->tag) sink_claim(s);
        return 1;
    }
    if (g_prefer) {
        int present = 0;
        for (int i = 0; i < g_nsrc; i++) if (is_preferred(&g_src[i])) present = 1;
        if (present) {
            if (!is_preferred(s)) return 0;
            // The preferred backend takes the output at once — unless another
            // stream of the same backend already has it, and then the plain
            // rules below decide between the two.
            if (!is_preferred(src_find(g_owner))) { sink_claim(s); return 1; }
        }
    }
    if (g_owner == s->tag) return 1;
    if (!peak) return 0;                    // silence neither claims nor steals
    // A LOSER that is producing sound is the one case this design gets wrong:
    // both backends are audible and we are throwing one away. Nothing here can
    // decide which is more important, so say it once, loudly, with the knobs
    // that override the choice.
    if (g_owner) {
        static const void *said;
        if (said != s->tag) {
            said = s->tag;
            fprintf(stderr, "  [au] '%s' is producing AUDIBLE audio but '%s' "
                            "owns the output, so '%s' is being dropped. If both "
                            "are meant to be heard, KL_AUDIO_MIX_SOURCES=1 mixes "
                            "them; KL_AUDIO_SOURCE=%s picks the other one.\n",
                    s->name, src_find(g_owner) ? src_find(g_owner)->name : "?",
                    s->name, s->name);
        }
    }
    if (!g_owner) { sink_claim(s); return 1; }
    kl_src *o = src_find(g_owner);
    if (!o) { sink_claim(s); return 1; }
    if (now_ns() - o->lit_ns > (uint64_t)g_steal_ms * 1000000ull) {
        sink_claim(s);
        return 1;
    }
    return 0;
}

// A dropped buffer still has to take as long as it would have taken to play,
// or the guest's mixer thread free-runs at whatever rate this loop can spin —
// which is a core at 100% producing audio nobody hears.
static void pace(size_t frames, unsigned rate) {
    if (!rate) rate = 48000;
    uint64_t us = (uint64_t)frames * 1000000ull / rate;
    if (us > 100000) us = 100000;
    if (us) nap_ms((unsigned)(us / 1000) ? (unsigned)(us / 1000) : 1);
}

void kl_audio_reporter(void (*fn)(FILE *)) {
    if (!fn) return;
    pthread_mutex_lock(&g_prod);
    for (int i = 0; i < g_nreport; i++)
        if (g_report[i] == fn) { pthread_mutex_unlock(&g_prod); return; }
    if (g_nreport < (int)(sizeof g_report / sizeof g_report[0]))
        g_report[g_nreport++] = fn;
    pthread_mutex_unlock(&g_prod);
}

int kl_audio_open_src_fmt(const void *tag, const char *name,
                          unsigned rate, unsigned channels, kl_audio_fmt fmt) {
    sink_env();
    pthread_mutex_lock(&g_prod);
    (void)src_add(tag, name, rate, channels, fmt);
    // Only the owner's format may rebuild the unit. Without this, the second
    // backend's open() tore the device down and built it again at ITS rate
    // mid-run — one restart, one audible gap, and the first backend's PCM
    // resampled at the second's ratio from then on. On hardware that was the
    // 44100 -> 48000 rebuild twelve lines after the AAudio stream appeared.
    int defer = (g_open && g_nsrc > 1 && g_owner != tag);
    pthread_mutex_unlock(&g_prod);
    if (defer) return 0;
    return sink_open(rate, channels, fmt);
}

// The bits-per-sample spelling, for callers that only know a width. Signed
// integer is assumed at every width except 8, which is unsigned — that is what
// a bare width means to both SLDataFormat_PCM and AAudio's PCM_I*.
int kl_audio_open_src(const void *tag, const char *name,
                      unsigned rate, unsigned channels, unsigned bits) {
    kl_audio_fmt fmt;
    if (kl_audio_fmt_from_bits(bits, &fmt) != 0) {
        fprintf(stderr, "  [au] refusing guest format %u Hz / %u ch / %u bit "
                        "(no such integer width)\n", rate, channels, bits);
        return -1;
    }
    return kl_audio_open_src_fmt(tag, name, rate, channels, fmt);
}

void kl_audio_close_src(const void *tag) {
    pthread_mutex_lock(&g_prod);
    kl_src *s = src_find(tag);
    int owned = (!g_owner || g_owner == tag);
    if (s) {
        int i = (int)(s - g_src);
        for (int j = i; j + 1 < g_nsrc; j++) g_src[j] = g_src[j + 1];
        g_nsrc--;
    }
    if (g_owner == tag) g_owner = NULL;     // the survivor can claim on its next lit buffer
    // The device stays open while another stream still uses it — an OpenSL
    // player beside the AAudio stream that closed, the second of Steam Link's
    // pair. Closing it under them left a survivor that then claimed it silent:
    // claiming a closed device flushed nothing and opened nothing.
    int last = (g_nsrc == 0);
    pthread_mutex_unlock(&g_prod);
    src_slot_release(tag);
    if (owned && last) kl_audio_close();
}

// The untagged spellings, kept for callers that only ever have one stream.
int kl_audio_open_fmt(unsigned rate, unsigned channels, kl_audio_fmt fmt) {
    return sink_open(rate, channels, fmt);
}

int kl_audio_open(unsigned rate, unsigned channels, unsigned bits) {
    kl_audio_fmt fmt;
    if (kl_audio_fmt_from_bits(bits, &fmt) != 0) {
        fprintf(stderr, "  [au] refusing guest format %u Hz / %u ch / %u bit "
                        "(no such integer width)\n", rate, channels, bits);
        return -1;
    }
    return sink_open(rate, channels, fmt);
}

// Whether a source other than `tag` is between its play and its pause.
// Caller holds g_prod.
static int other_playing(const void *tag) {
    for (int i = 0; i < g_nsrc; i++)
        if (g_src[i].tag != tag && g_src[i].playing) return 1;
    return 0;
}

void kl_audio_play_src(const void *tag) {
    pthread_mutex_lock(&g_prod);
    kl_src *s = src_find(tag);
    if (s) s->playing = 1;
    int go = !g_owner || g_owner == tag;
    pthread_mutex_unlock(&g_prod);
    if (go) kl_audio_play();
}

// While mixing, one stream stopping must not stop the device under the other.
void kl_audio_pause_src(const void *tag) {
    pthread_mutex_lock(&g_prod);
    kl_src *s = src_find(tag);
    if (s) s->playing = 0;
    int go = g_mix_all ? !other_playing(tag) : (!g_owner || g_owner == tag);
    pthread_mutex_unlock(&g_prod);
    if (go) kl_audio_pause();
}

// Flushing drops what is queued and not yet played. Without a mix (one owner)
// the owner's flush is the device's, as it always was. While mixing, the ring
// holds every playing stream's audio summed together, so a flush from one of
// them may only drop the ring when no OTHER stream is playing; otherwise the
// ring is left alone and only this stream's own slot — its write cursor and
// resampler state — is released, so its next write starts again at the reader.
void kl_audio_flush_src(const void *tag) {
    pthread_mutex_lock(&g_prod);
    int mixed_alone = g_mix_all && !other_playing(tag);
    int go = g_mix_all ? mixed_alone : (!g_owner || g_owner == tag);
    pthread_mutex_unlock(&g_prod);
    if (go) kl_audio_flush();
    if (g_mix_all) src_slot_release(tag);
}

void kl_audio_close(void) {
    // Before the locks: a writer waiting for room holds g_wlock, and a stopped
    // device is what lets it out (see ring_put).
    g_playing = 0;
    pthread_mutex_lock(&g_lock);
    pthread_mutex_lock(&g_wlock);
    unit_destroy();
    // NOT dump_close(): FMOD destroys and re-creates the player during startup,
    // so closing here ended the capture 0.12 s into a 12.8 s run. The capture is
    // per-process and the report closes it.
    g_open = 0;
    free(g_ring);   g_ring = NULL;   g_cap = 0;
    free(g_scratch); g_scratch = NULL; g_scratch_frames = 0;
    free(g_inbuf);   g_inbuf = NULL;   g_inbuf_samples = 0;
    atomic_store(&g_w, 0); atomic_store(&g_r, 0);
    g_ring_gen++;
    pthread_mutex_unlock(&g_wlock);
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
    pthread_mutex_lock(&g_wlock);           // the resampler state is a writer's
    int was = g_running;
    if (was && g_unit) { AudioOutputUnitStop(g_unit); g_running = 0; }
    atomic_store(&g_r, atomic_load(&g_w));
    g_pos = 0.0;
    memset(g_prev, 0, sizeof g_prev);
    if (was) unit_start();
    pthread_mutex_unlock(&g_wlock);
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

// The scene saying the app is off screen (crown press / immersive dismissal) —
// a DELIBERATE silence, not an OS interruption. The distinction has to exist in
// C because the watchdog thread's stuck-interruption escape force-resumes a
// latched g_interrupted after a few seconds: with no latch of its own, a crown
// press got exactly that — the music resumed while backgrounded, and a rebuild
// mid-transition once measured the wrong rate (slo-mo playback). With this set,
// nothing recovers the stream until the matching unsilence.
void kl_audio_set_silenced(int silenced) {
    g_silenced = silenced;
    kl_audio_interrupted(silenced);
}

// Rebuild the unit. Used by both the notification path and the watchdog, and it
// has to be a full rebuild rather than a stop/start: a route change can leave
// the unit initialised against a sample rate the hardware no longer has, and
// that unit will start successfully and never call back.
int kl_audio_restart(void) {
    if (!g_open) return -1;
    pthread_mutex_lock(&g_lock);
    if (!g_open) { pthread_mutex_unlock(&g_lock); return -1; }   // closed meanwhile
    pthread_mutex_lock(&g_wlock);           // the ring may be replaced below
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
                g_ring_gen++;
            }
        }
        atomic_store(&g_r, atomic_load(&g_w));
        g_pos = 0.0;
        memset(g_prev, 0, sizeof g_prev);
        unit_start();
    }
    pthread_mutex_unlock(&g_wlock);
    pthread_mutex_unlock(&g_lock);
    g_restarts++;
    // Rate-limited for the same reason the watchdog's line is, and with the
    // same exception: a FAILURE is always said. A successful restart repeated
    // thirty times a second says nothing the count in kl_audio_report does not.
    static uint64_t last_ok_ns;
    uint64_t now = now_ns();
    if (rc != 0 || now - last_ok_ns > 5000ull * 1000000ull) {
        fprintf(stderr, "  [au] restart %s (device now %.0f Hz, %lu restart(s) "
                        "this run)\n",
                rc == 0 ? "ok" : "FAILED", g_out_rate, g_restarts);
        if (rc == 0) last_ok_ns = now;
    }
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
// float32 interleaved at the guest rate -> float32 interleaved at the device
// rate, with linear interpolation across the chunk boundary. The caller has
// already widened the guest's encoding (kl_audio_to_float), so this loop never
// has to know whether the guest was FMOD's int16 or Wwise's float. The virtual
// input stream is v[-1] = the last frame of the previous chunk, v[0..n-1] =
// this one, so g_pos carries fractional position between calls and a rate ratio
// of exactly 1.0 degenerates to a straight copy.
//
// `*buf_samples` is the buffer's size in SAMPLES, not frames: a buffer that
// outlives a reopen (a mixed source's, kept per thread) was sized for the old
// channel count, and a mono device reopened as stereo needs twice the floats
// for the same number of frames.
static size_t resample(const float *src, size_t n, unsigned in_rate, unsigned ich,
                       double *posp, float *prev, float **buf, size_t *buf_samples,
                       float **out) {
    double ratio = (double)in_rate / g_out_rate;
    size_t max_out = (size_t)((double)(n + 2) / ratio) + 2;
    if (max_out * g_out_ch > *buf_samples) {
        float *p = realloc(*buf, max_out * g_out_ch * sizeof(float));
        if (!p) return 0;
        *buf = p;
        *buf_samples = max_out * g_out_ch;
    }
    float *dst = *buf;
    unsigned och = g_out_ch;
    double pos = *posp;
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
        ((idx) < 0 ? prev[ch] \
                   : src[(size_t)((idx) < (long)n ? (idx) : (long)n - 1) * ich \
                         + ((ch) < ich ? (ch) : 0)])

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
            prev[c] = src[(n - 1) * ich + (c < ich ? c : 0)];
    }
    pos -= (double)n;
    if (pos < -1.0) pos = -1.0;
    *posp = pos;
    *out = dst;
    return produced;
}

// The owner's stream, at the format the device was opened for.
static size_t convert(const float *src, size_t n, float **out) {
    return resample(src, n, g_in_rate, g_in_ch, &g_pos, g_prev,
                    &g_scratch, &g_scratch_frames, out);
}

// The watchdog. A render callback that has stopped arriving while we believe we
// are running is the visionOS failure mode this file was written expecting:
// the OS reclaims the stream on a route change or a scene transition and the
// unit stays nominally started.
//
static void report_body(FILE *f);
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
    // A FLOOR under the printing, independent of the restart rate.
    //
    // The backoff keys on a CONSECUTIVE streak, and a stream that alternates
    // healthy/unhealthy resets it every other check — so on Tetris Effect this
    // printed 4,746 times in one run on device, two lines per restart, and
    // buried every other [au] line in the log including the ones that would say
    // why. The streak still governs the restart RATE; a wall-clock floor now
    // governs the print rate independently, and the line still names what the
    // unit's own bookkeeping thought at the time: with the callback stopped,
    // g_running=1 is the OS having taken the stream and g_running=0 is the unit
    // having been stopped underneath us (a flush, an interruption, a stop that
    // never came back). Those are different bugs and the old line could not
    // tell them apart.
    static uint64_t last_say_ns;
    static unsigned long quiet;
    uint64_t now = now_ns();
    if (g_wd_streak < 4 || now - last_say_ns > 5000ull * 1000000ull) {
        if (quiet)
            fprintf(stderr, "  [au] ...%lu further restart(s) went unsaid\n", quiet);
        fprintf(stderr, "  [au] %s (%.2f s since the last render callback) — "
                        "restarting\n",
                g_running ? "the render callback has stopped"
                          : "the unit is not running",
                (double)idle / 1e9);
        last_say_ns = now;
        quiet = 0;
    } else {
        quiet++;
    }
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
        // call, another app taking the route) is not fought over. And not when
        // the silence is deliberate (g_silenced — the scene is off screen):
        // that is the one state where resuming is wrong by definition.
        if (g_interrupted && g_playing && !g_silenced) {
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
        // The report, DURING the run.
        //
        // kl_audio_report holds the one number that settles "why is there no
        // sound": whether the guest's PCM was ever anything but zero. It never
        // prints on hardware, because it runs from the exit path and a headset
        // app is killed rather than exited. So it is printed every
        // KL_AUDIO_REPORT_SECS (default 15, 0 disables), on a thread that is
        // already awake.
        {
            static int secs = -1;
            static uint64_t last;
            if (secs < 0) secs = kl_env_int("KL_AUDIO_REPORT_SECS", 15);
            if (secs > 0) {
                uint64_t now = now_ns();
                if (!last) last = now;
                else if (now - last >= (uint64_t)secs * 1000000000ull) {
                    last = now;
                    fprintf(stderr, "  [au] --- live ---\n");
                    report_body(stderr);
                }
            }
        }
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
    // A deliberate scene silence (g_silenced) outranks this: the crown-press
    // dismissal must stay silent even if a pause/running transition fires on
    // the way. The scene's unsilence clears the latch before calling this.
    if (g_silenced) return;
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

// Where the reader IS, for pacing, rather than where it last jumped to. The
// device takes its frames a whole IO buffer at a time (10 ms here — visionOS
// would not grant 5), so the read cursor, and with it every write that waits
// for room, moves in 10 ms steps: FMOD's 5.3 ms blocks were taken back two at a
// time, its mixer clock advanced in 10 ms jumps, and Unity's VideoPlayer —
// which times a clip with sound by that clock — took frames a refresh late
// several times a second at 100 Hz (LES MILLS BODYCOMBAT's trainer clips,
// 2026-10-15). Between two callbacks the device is playing at its rate, so the
// position is carried forward by the clock, never past the frames that
// callback took. Only the room a writer waits for uses it; the guard against
// writing in the past keeps the real cursor. KL_AUDIO_SMOOTH=0|1.
static uint64_t pacing_reader(uint64_t r) {
    if (!g_smooth) return r;
    uint64_t t = atomic_load_explicit(&g_last_render_ns, memory_order_relaxed);
    uint64_t n = atomic_load_explicit(&g_last_render_frames, memory_order_relaxed);
    uint64_t now = now_ns();
    if (!t || now <= t) return r;
    double rate = g_out_rate > 0 ? g_out_rate : 48000.0;
    uint64_t adv = (uint64_t)((double)(now - t) * rate / 1e9);
    return r + (adv < n ? adv : n);
}

// Whether the render callback has stopped arriving while the guest wants sound
// — the condition watchdog() restarts on, read without acting on it.
static int render_stalled(void) {
    if (!g_open || g_interrupted || !g_playing) return 0;
    uint64_t idle = now_ns() - atomic_load_explicit(&g_last_render_ns, memory_order_relaxed);
    uint64_t limit = (g_wd_streak > 3 ? 3000ull : 500ull) * 1000000ull;
    return idle > limit;
}

// Put `frames` converted frames into the ring for `src`, blocking until there
// is room — THIS is the clock. Caller holds g_wlock. With `unlocked` the 2 ms
// wait for room happens outside g_wlock (KL_AUDIO_MIX_UNLOCK), so `conv` must
// be the caller's own memory, and `*slot` is found again afterwards because a
// release can shift the table meanwhile; if the ring was freed or replaced
// during the wait (g_ring_gen), the write ends there.
//
// A dead render callback ends the wait too, with *stalled set: the caller runs
// watchdog() once it has dropped its locks. Calling it from here would take
// g_lock under g_wlock, the reverse of the order (see the lock comment at the
// top), and a close or reopen waiting for g_wlock would deadlock against it.
static size_t ring_put(const void *src, int *slot, const float *conv, size_t frames,
                       int unlocked, int *stalled) {
    *stalled = 0;
    const uint64_t gen = g_ring_gen;
    // The target has to be able to hold at least two of these, or the wait
    // below can never be satisfied and every write times out.
    if (g_target < frames * 2) {
        g_target = frames * 2;
        if (g_target > g_cap - 1) g_target = g_cap - 1;
    }

    // **One source takes the shared write cursor, and that is the whole of the
    // single-producer path.** The per-source cursor and the mix exist for a
    // guest with TWO output streams (Steam Link opens a float one and an int16
    // one); every other guest has exactly one, and for it `g_w` IS that
    // source's position by definition. Deriving it from a second cursor instead
    // was measured to starve the producer outright: Beat Saber went from 13692
    // writes with 0 underruns to 78 writes with 8599, i.e. the guest's mixer —
    // which is paced by this call — stalled. The equivalence was argued rather
    // than constructed; this constructs it.
    int mixing = g_src_n > 1;
    uint64_t r0 = atomic_load_explicit(&g_r, memory_order_acquire);
    uint64_t pos;
    if (!mixing) {
        pos = atomic_load_explicit(&g_w, memory_order_relaxed);
    } else {
        pos = g_src_pos[*slot];
        // (Re)sync a new or stalled source to the latency target ahead of the
        // reader, so its samples mix with the others in step — not in the past
        // (already played as silence) nor so far ahead the ring wraps under it.
        if (pos < r0 || pos > r0 + g_cap) {
            uint64_t back = g_target > frames ? g_target - frames : 0;
            if (pos) g_src_resyncs[*slot]++;          // 0 is a new source, not a fall
            pos = r0 + back;
        }
    }
    unlocked = unlocked && mixing;

    uint64_t deadline = now_ns() + 2000ull * 1000000ull;
    size_t written = 0;
    int gone = 0;
    while (written < frames) {
        if (g_ring_gen != gen || !g_ring) { gone = 1; break; }   // freed or replaced during a wait
        uint64_t r = atomic_load_explicit(&g_r, memory_order_acquire);
        uint64_t here = pos + written;
        if (here < r) here = r;                       // never write in the past
        uint64_t rp = pacing_reader(r);
        uint64_t ahead = here > rp ? here - rp : 0;
        size_t room = ahead < g_target ? (size_t)(g_target - ahead) : 0;
        if (ahead < g_cap && g_cap - (size_t)ahead < room) room = g_cap - (size_t)ahead;
        size_t take = frames - written;
        if (take > room) take = room;

        if (take) {
            if (!mixing) {
                // A COPY, in runs, exactly as the single-producer path always
                // did: with one source there is nothing to sum with, and a
                // read-modify-write of every sample is both slower and only
                // correct if the slot is already silent.
                for (size_t done = 0; done < take; ) {
                    size_t off = (size_t)((here + done) % g_cap);
                    size_t run = g_cap - off;
                    if (run > take - done) run = take - done;
                    memcpy(g_ring + off * g_out_ch,
                           conv + (written + done) * g_out_ch,
                           run * g_out_ch * sizeof(float));
                    done += run;
                }
            } else {
                for (size_t k = 0; k < take; k++) {
                    size_t off = (size_t)((here + k) % g_cap);
                    const float *sp = conv + (written + k) * g_out_ch;
                    float *dp = g_ring + off * g_out_ch;
                    for (unsigned c = 0; c < g_out_ch; c++) dp[c] += sp[c];   // MIX
                }
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
        if (render_stalled()) { *stalled = 1; break; }
        if (now_ns() > deadline) { g_short_writes++; break; }
        if (unlocked) {
            pthread_mutex_unlock(&g_wlock);
            nap_ms(2);
            pthread_mutex_lock(&g_wlock);
        } else {
            nap_ms(g_smooth ? 1 : 2);
        }
    }
    if (unlocked) *slot = src_slot(src);
    if (gone) {
        // Cursors of a ring that no longer exists: start again at the reader.
        g_src_pos[*slot] = 0;
        return written;
    }
    g_src_pos[*slot] = pos + written < r0 ? r0 : pos + written;
    g_src_frames[*slot] += written;
    // A source that appears LATE must not inherit a cursor from the
    // single-source era: the moment a second one registers, both are resynced
    // against the reader on their next write by the ladder above.
    if (!mixing && g_src_n > 1) g_src_pos[*slot] = 0;
    return written;
}

// The per-source share of real time, one line per source, while more than one
// is writing. Caller holds g_wlock.
static void src_lines(FILE *f) {
    if (g_src_n < 2) return;
    uint64_t played = atomic_load(&g_frames_played);
    for (int i = 0; i < g_src_n; i++) {
        uint64_t span = played - g_src_since[i];
        fprintf(f, "  [au]   src%d: %.0f%% of real time since it came, %lu resync(s)\n",
                i, span ? 100.0 * (double)g_src_frames[i] / (double)span : 0.0,
                g_src_resyncs[i]);
    }
}

// What every write that reached the ring is followed by: the PCM capture, the
// trace, and the periodic verdict. Caller holds g_wlock.
static void after_write(int slot, size_t n, size_t frames, const float *conv,
                        size_t written, int peak) {
    // Armed by the first LIT buffer and never disarmed: the point of the cap is
    // to record the first N seconds of audible output, and a capture that
    // started at the first write would be the boot window's silence. While
    // mixing it records the first source's share only.
    if (!g_dump_armed && peak) g_dump_armed = 1;
    if (g_dump_armed && (!g_mix_all || slot == 0)) dump_write(conv, written);

    if (g_trace && (g_writes % 200) == 1) {
        size_t fill = atomic_load(&g_w) - atomic_load(&g_r);
        fprintf(stderr, "  [au] write #%lu src%d: %zu in -> %zu out, fill %zu/%zu "
                        "(%.1f ms), %d src(s), underruns %u\n",
                g_writes, slot, n, frames, fill, g_target,
                1000.0 * (double)fill / g_out_rate, g_src_n,
                atomic_load(&g_underruns));
    }

    // Periodic peak verdict, WITHOUT the trace flag: "can I hear anything?" is a
    // question a normal run has to be able to answer. A peak of 0 across hundreds
    // of buffers means the guest's mixer is running but producing silence (banks
    // not loaded, no events) — a different bug from an output that never opened.
    if ((g_writes % 1000) == 1) {
        size_t fill = atomic_load(&g_w) - atomic_load(&g_r);
        fprintf(stderr, "  [au] guest PCM so far: peak %d/32767, %lu of %lu "
                        "buffers all-zero — %s; fill %.1f ms, %u underruns, "
                        "%lu short writes, %lu restarts\n",
                g_peak, g_silent_buffers, g_writes,
                g_peak ? "producing audio" : "ONLY SILENCE so far",
                g_out_rate > 0 ? 1000.0 * (double)fill / g_out_rate : 0.0,
                atomic_load(&g_underruns), g_short_writes, g_restarts);
        src_lines(stderr);
    }
}

// The mixed path (KL_AUDIO_MIX_SOURCES): every source reaches the ring, so each
// is widened from ITS format and resampled from ITS rate and channel count with
// its own resampler position. The single-producer path below cannot do that —
// it widens with the owner's format, which read the second of Steam Link's
// streams (float beside int16) as the wrong encoding — and its try-lock turns a
// second writer away, which is right for a non-owner and wrong for a mix.
// Conversion output is this thread's own, so the wait for room may happen
// outside g_wlock.
//
// "This thread's own" is a per-thread pair of buffers behind a pthread key, so
// they are freed when the thread ends: a guest that starts a feeder thread per
// stream and opens streams over and over would otherwise leak a pair each time.
typedef struct { float *in, *out; size_t in_n, out_n; } mix_bufs;   // sizes in samples
static pthread_key_t  g_mix_key;
static pthread_once_t g_mix_once = PTHREAD_ONCE_INIT;
static void mix_bufs_free(void *p) {
    mix_bufs *b = p;
    if (!b) return;
    free(b->in);
    free(b->out);
    free(b);
}
static int g_mix_key_ok;
static void mix_key_make(void) { g_mix_key_ok = kl_key_create(&g_mix_key, mix_bufs_free, "the audio mix buffers"); }
static mix_bufs *mix_bufs_get(void) {
    pthread_once(&g_mix_once, mix_key_make);
    if (!g_mix_key_ok) return NULL;   // no buffers: mix_write drops the block
    mix_bufs *b = pthread_getspecific(g_mix_key);
    if (!b) {
        b = calloc(1, sizeof *b);
        if (b && pthread_setspecific(g_mix_key, b) != 0) { free(b); b = NULL; }
    }
    return b;
}

static size_t mix_write(const void *src, const void *pcm, size_t bytes, size_t sn,
                        unsigned sch, unsigned srate, kl_audio_fmt sfmt, int peak) {
    mix_bufs *t = mix_bufs_get();
    if (!t) return 0;
    size_t samples = sn * sch;
    if (samples > t->in_n) {
        float *fp = realloc(t->in, samples * sizeof(float));
        if (!fp) return 0;
        t->in = fp;
        t->in_n = samples;
    }
    kl_audio_to_float(sfmt, pcm, t->in, samples);

    pthread_mutex_lock(&g_wlock);
    if (!g_open || !g_ring || !(g_out_rate > 0.0)) { pthread_mutex_unlock(&g_wlock); return 0; }
    if (peak > g_peak) g_peak = peak;
    if (!peak) g_silent_buffers++;

    int slot = src_slot(src);
    float *conv = NULL;
    size_t frames = resample(t->in, sn, srate ? srate : g_in_rate, sch,
                             &g_src_rpos[slot], g_src_prev[slot],
                             &t->out, &t->out_n, &conv);
    if (!frames) { pthread_mutex_unlock(&g_wlock); return 0; }
    g_writes++;

    int stalled;
    size_t written = ring_put(src, &slot, conv, frames, g_mix_unlock, &stalled);
    after_write(slot, sn, frames, conv, written, peak);
    pthread_mutex_unlock(&g_wlock);
    if (stalled) watchdog();                // with no lock held: it takes g_lock
    if (written == frames) return bytes;
    size_t in_frames = (size_t)((double)sn * (double)written / (double)frames);
    return in_frames * sch * kl_audio_fmt_bytes(sfmt);
}

size_t kl_audio_write_src(const void *src, const void *pcm, size_t bytes) {
    if (!g_open || !pcm || !bytes) return 0;
    // The source's OWN format, not the sink's: a buffer that is about to be
    // dropped still has to be measured and paced in the terms its producer
    // sent it, and those are not the terms the device was opened against.
    pthread_mutex_lock(&g_prod);
    kl_src *s = src_find(src);
    unsigned sch   = (s && s->ch)   ? s->ch   : g_in_ch;
    unsigned srate = (s && s->rate) ? s->rate : g_in_rate;
    kl_audio_fmt sfmt = s ? s->fmt : g_in_fmt;
    if (!sch) sch = 2;
    size_t sn = bytes / (sch * kl_audio_fmt_bytes(sfmt));
    if (!sn) { pthread_mutex_unlock(&g_prod); return 0; }

    // The peak, measured in the SOURCE's encoding. Widened a chunk at a time
    // on the stack rather than through g_inbuf: this runs under g_prod, the
    // conversion below runs under g_write, and two producers sharing one
    // realloc'd buffer across two different locks is a use-after-free waiting
    // for the second Steam Link stream. See kl_audio_to_float.
    int peak = 0;
    {
        float tmp[512];
        unsigned bps = kl_audio_fmt_bytes(sfmt);
        size_t left = sn * sch, off = 0;
        while (left) {
            size_t c = left > 512 ? 512 : left;
            kl_audio_to_float(sfmt, (const unsigned char *)pcm + off * bps, tmp, c);
            for (size_t i = 0; i < c; i++) {
                float a = tmp[i] < 0.0f ? -tmp[i] : tmp[i];
                // abs() of INT16_MIN is 32768, one past the positive range.
                // Clamp it, or the report claims a peak the format cannot hold
                // (it read "peak 32768/32767" on hardware, which looked like
                // clipping and was not).
                int v = (int)lrintf(a * 32768.0f);
                if (v > 32767) v = 32767;
                if (v > peak) peak = v;
            }
            off += c; left -= c;
        }
    }
    if (s) {
        s->writes++;
        if (peak) { s->lit_ns = now_ns(); if (peak > s->peak) s->peak = peak; }
        else       s->silent++;
    }

    int mine = arbitrate(s, peak);
    int mixed = g_mix_all && s;
    pthread_mutex_unlock(&g_prod);

    if (!mine) { pace(sn, srate); return bytes; }
    if (mixed) return mix_write(src, pcm, bytes, sn, sch, srate, sfmt, peak);

    // Past here is the original single-producer path — one resampler position,
    // one realloc'd scratch buffer, one write cursor — and `g_write` is what
    // keeps that description true across the instant ownership changes hands.
    // TRY-lock, not lock: this body blocks for a buffer's worth of playback by
    // design, so a second thread arriving here must be turned away rather than
    // queued behind it. Turning it away is exactly what a non-owner gets.
    if (pthread_mutex_trylock(&g_write) != 0) { pace(sn, srate); return bytes; }

    // The convert()/scratch state and the mix cursors are shared, so the whole
    // body is serialised. The reader never takes this lock. The device's
    // format is read under it: a reopen changes it under g_wlock too.
    pthread_mutex_lock(&g_wlock);
    if (!g_open || !g_ring || !g_in_ch) {
        pthread_mutex_unlock(&g_wlock);
        pthread_mutex_unlock(&g_write);
        return 0;
    }
    unsigned in_frame = g_in_ch * kl_audio_fmt_bytes(g_in_fmt);
    size_t n = bytes / in_frame;
    if (!n) {
        pthread_mutex_unlock(&g_wlock);
        pthread_mutex_unlock(&g_write);
        return 0;
    }

    if (peak > g_peak) g_peak = peak;
    if (!peak) g_silent_buffers++;

    // Widen the guest's encoding to float before the resampler touches it. The
    // whole of "static in one ear, half-speed voices in the other" on Batman:
    // Arkham Shadow was this step being absent for a float32 guest: the bytes went straight
    // into an int16 resampler, which read two samples out of every float.
    // Under g_wlock, which is the lock convert()'s own scratch state is under.
    size_t samples = n * g_in_ch;
    if (samples > g_inbuf_samples) {
        float *fp = realloc(g_inbuf, samples * sizeof(float));
        if (!fp) { pthread_mutex_unlock(&g_wlock); pthread_mutex_unlock(&g_write); return 0; }
        g_inbuf = fp;
        g_inbuf_samples = samples;
    }
    kl_audio_to_float(g_in_fmt, pcm, g_inbuf, samples);

    float *conv = NULL;
    size_t frames = convert(g_inbuf, n, &conv);
    if (!frames) {
        pthread_mutex_unlock(&g_wlock);
        pthread_mutex_unlock(&g_write);
        return 0;
    }
    g_writes++;

    // conv is the shared scratch, so never an unlocked wait here.
    int slot = src_slot(src), stalled;
    size_t written = ring_put(src, &slot, conv, frames, 0, &stalled);
    after_write(slot, n, frames, conv, written, peak);
    pthread_mutex_unlock(&g_wlock);
    pthread_mutex_unlock(&g_write);
    if (stalled) watchdog();                // with no lock held: it takes g_lock
    return written == frames ? bytes : written * in_frame;
}

unsigned kl_audio_underruns(void) { return atomic_load(&g_underruns); }

// How much audio is already queued ahead of the render callback, in ms.
//
// This is the one number a PULL-side producer needs in order to spend wall time
// safely. kl_audio_write is the clock: it blocks until the fill is back at
// g_target, so in steady state this ring holds KL_AUDIO_LATENCY_MS (80 ms) of
// audio the device has not played yet. A feeder that pauses for a few
// milliseconds is therefore not a gap in the output — it is spending lead that
// already exists, and the output only breaks if it spends more lead than there
// is. See kl_aaudio.c's feeder, which is the caller and the reason this exists.
//
// -1 means "there is no lead to spend", and that reading is deliberately
// conservative: no device, a stopped or interrupted unit, or a caller that is
// not the OWNER — a non-owner's PCM is paced and dropped, so this ring's fill
// says nothing whatever about it.
double kl_audio_lead_ms(const void *tag) {
    if (!g_open || !g_running || !g_playing || g_interrupted) return -1.0;
    if (!g_ring || !(g_out_rate > 0.0)) return -1.0;
    if (tag && g_owner != tag) return -1.0;
    size_t w = atomic_load_explicit(&g_w, memory_order_acquire);
    size_t r = atomic_load_explicit(&g_r, memory_order_relaxed);
    return 1000.0 * (double)(w - r) / g_out_rate;
}

// The body, without the teardown. Split out because the watchdog thread prints
// this every 15 s DURING the run (the exit path is unreachable on device), and
// closing the PCM dump on the first live report would leave the rest of the run
// undumped — an instrument quietly disabling another one.
static void report_body(FILE *f) {
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
    // Per backend, so "35% of buffers were silent" can never again be a single
    // number that hides two producers fighting over one ring. `owns` is the one
    // whose PCM reached the device; the others were paced and dropped.
    for (int i = 0; i < g_nsrc; i++) {
        const kl_src *s = &g_src[i];
        fprintf(f, "    source '%s' %u Hz/%u ch: %lu buffer(s), %lu silent, "
                   "peak %d — %s\n",
                s->name, s->rate, s->ch, s->writes, s->silent, s->peak,
                g_mix_all ? "MIXED into the output"
                : g_owner == s->tag ? "OWNS the output" : "paced and dropped");
    }
    // ...and, while more than one is mixed, whether each keeps up with real
    // time. A try-lock: a writer can hold g_wlock for a whole wait, and this is
    // also reached from the fault handler.
    if (pthread_mutex_trylock(&g_wlock) == 0) {
        src_lines(f);
        pthread_mutex_unlock(&g_wlock);
    }
    // ...and then whatever the producer backends want to say about the SAME
    // 15 s. "16062 of 47070 buffers silent" cannot say why on its own, because
    // why is a fact about the guest's callback and this file never sees one. A backend registers itself at open; nothing is printed if none
    // did, and a mains that links the sink without a backend is unchanged.
    for (int i = 0; i < g_nreport; i++)
        if (g_report[i]) g_report[i](f);
    // The capture twin of the line above: it ran only if a guest opened an input
    // stream with the mic toggle on, so it is silent (no line) on the common
    // playback-only run.
    uint64_t cap = atomic_load(&g_mic_frames_cap);
    if (g_mic_enabled || cap) {
        int mpk = atomic_load(&g_mic_peak);
        fprintf(f, "  microphone: %.2f s captured, %.2f s read by guest, %.2f s dropped "
                   "(ring overrun), peak %d/32767 — %s\n",
                g_mic_rate ? (double)cap / g_mic_rate : 0.0,
                g_mic_rate ? (double)atomic_load(&g_mic_frames_read) / g_mic_rate : 0.0,
                g_mic_rate ? (double)atomic_load(&g_mic_frames_drop) / g_mic_rate : 0.0,
                mpk, mpk ? "the microphone heard audio" : "THE MICROPHONE HEARD ONLY SILENCE");
    }
}

void kl_audio_report(FILE *f) {
    if (!g_writes && !g_open) return;
    dump_close();   // the report is the last thing a clean run does
    report_body(f);
}

// The same numbers, on one line and with no latch. The report above has never
// once been read on device: a port in a headset is killed, it does not return
// from main, so the only telemetry that survives is telemetry printed while the
// thing is still running.
void kl_audio_heartbeat(FILE *f) {
    fprintf(f, "  [au] heartbeat: unit open=%d running=%d playing=%d interrupted=%d | "
               "in %u Hz/%u ch/%s | "
               "%lu writes (%lu short, %lu all-zero), peak %d/32767, "
               "%.2f s played, %u underruns — %s\n",
            g_open, g_running, g_playing, g_interrupted,
            g_in_rate, g_in_ch, kl_audio_fmt_name(g_in_fmt),
            g_writes, g_short_writes, g_silent_buffers, g_peak,
            g_out_rate > 0 ? (double)atomic_load(&g_frames_played) / g_out_rate : 0.0,
            atomic_load(&g_underruns),
            !g_writes ? "NOTHING WRITTEN — the guest never handed us PCM"
          : !g_peak   ? "THE GUEST IS WRITING PURE SILENCE"
                      : "the guest produced audio");
}


// ===========================================================================
// Microphone capture — a second, independent CoreAudio unit.
//
// Everything below is the capture path, deliberately walled off from the output
// unit: its own component instance (g_cap_unit), its own ring, its own lock. It
// touches none of the output state, so a guest that opens an input stream cannot
// change how playback sounds, and a run that never records pays nothing (the
// unit is not even created).
// ===========================================================================

// The device's capture rate, asked of the unit rather than assumed. On RemoteIO
// the INPUT scope of element 1 IS the hardware side; there is no sample-rate
// converter to lean on, so we capture at whatever it reports and resample on the
// consumer side (kl_aaudio.c), the same bargain the output unit strikes.
static double measure_input_rate(AudioUnit u) {
    AudioStreamBasicDescription hw;
    UInt32 sz = sizeof hw;
    if (AudioUnitGetProperty(u, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 1, &hw, &sz) == noErr &&
        hw.mSampleRate > 8000.0 && hw.mSampleRate < 400000.0)
        return hw.mSampleRate;
    return 0.0;
}

// The mic's NATIVE channel count. RemoteIO's input bus has no channel converter
// any more than a rate converter (see measure_device_rate): a client format that
// asks for fewer/more channels than the hardware delivers initialises without
// error and then renders SILENCE. So capture at the native count and let
// kl_aaudio.c fold/fan-out to whatever the guest asked for. 0 => unknown.
static unsigned measure_input_channels(AudioUnit u) {
    AudioStreamBasicDescription hw;
    UInt32 sz = sizeof hw;
    if (AudioUnitGetProperty(u, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 1, &hw, &sz) == noErr &&
        hw.mChannelsPerFrame >= 1 && hw.mChannelsPerFrame <= 8)
        return hw.mChannelsPerFrame;
    return 0;
}

// ---- the input callback ----
//
// Real-time, and the producer for the capture ring. AAudio's render callback is
// the twin of render_cb above with the arrow reversed: CoreAudio hands us an
// empty AudioBufferList slot (io is NULL for an input unit), we AudioUnitRender()
// the mic's frames into our fixed scratch, and copy them into the ring. No lock,
// no allocation, no guest code — the resample/convert the guest needs happens on
// its own reader thread.
static OSStatus capture_cb(void *ref, AudioUnitRenderActionFlags *flags,
                           const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                           AudioBufferList *io) {
    (void)ref; (void)io;
    AudioUnit u = g_cap_unit;
    if (!u || !g_mic_ring || !g_cap_scratch) return noErr;
    // The size CoreAudio asked for, before the scratch cap below: a burst that
    // changes (or is clipped) is part of what the heartbeat has to show.
    atomic_store_explicit(&g_mic_cb_frames, nframes, memory_order_relaxed);
    uint32_t lo = atomic_load_explicit(&g_mic_cb_min, memory_order_relaxed);
    if (!lo || nframes < lo) atomic_store_explicit(&g_mic_cb_min, nframes, memory_order_relaxed);
    if (nframes > atomic_load_explicit(&g_mic_cb_max, memory_order_relaxed))
        atomic_store_explicit(&g_mic_cb_max, nframes, memory_order_relaxed);
    // Never ask AudioUnitRender for more than the scratch can hold. iOS bursts
    // are far below this cap; a larger one is simply clipped rather than risking
    // a write past the buffer.
    if (nframes > g_cap_scratch_frames) nframes = (UInt32)g_cap_scratch_frames;

    g_cap_abl.mNumberBuffers = 1;
    g_cap_abl.mBuffers[0].mNumberChannels = g_mic_ch;
    g_cap_abl.mBuffers[0].mDataByteSize   = (UInt32)(nframes * g_mic_ch * sizeof(int16_t));
    g_cap_abl.mBuffers[0].mData           = g_cap_scratch;
    OSStatus s = AudioUnitRender(u, flags, ts, bus, nframes, &g_cap_abl);
    if (s != noErr) {               // a dropped render is a gap, not a fault to fight
        static int lg;              // ...but log the FIRST one: a persistent render
        if (!lg) { lg = 1; char b[8];   // failure (e.g. a client format the input
            fprintf(stderr, "  [au] mic AudioUnitRender failed: %s — capture will be "
                            "silent\n", fourcc(s, b)); }   // bus won't deliver) is silence
        return noErr;
    }

    // A cheap peak — the "did the mic hear anything" verdict the report prints,
    // and pure arithmetic, so it is fine on the real-time thread.
    int peak = 0;
    size_t ns = (size_t)nframes * g_mic_ch;
    for (size_t i = 0; i < ns; i++) {
        int v = g_cap_scratch[i] < 0 ? -g_cap_scratch[i] : g_cap_scratch[i];
        if (v > peak) peak = v;
    }
    if (peak > atomic_load_explicit(&g_mic_peak, memory_order_relaxed))
        atomic_store_explicit(&g_mic_peak, peak, memory_order_relaxed);
    // The per-line twin. A heartbeat resetting it between the load and the
    // store loses at most this one buffer's peak to the next line; no lock.
    if (peak > atomic_load_explicit(&g_mic_peak_iv, memory_order_relaxed))
        atomic_store_explicit(&g_mic_peak_iv, peak, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_mic_bufs, 1, memory_order_relaxed);
    if (peak >= 32767) atomic_fetch_add_explicit(&g_mic_bufs_clip, 1, memory_order_relaxed);

    // A one-shot verdict for the next log: is the callback actually HEARING
    // anything, or rendering silence? Fires at most twice over a session, so the
    // rare fprintf here is a diagnostic cost, not a steady-state one.
    {
        static _Atomic uint64_t seen; static _Atomic int said_live, said_quiet;
        uint64_t n = atomic_fetch_add_explicit(&seen, nframes, memory_order_relaxed) + nframes;
        if (peak > 200 && !atomic_exchange_explicit(&said_live, 1, memory_order_relaxed))
            fprintf(stderr, "  [au] mic capture LIVE: peak %d/%d — hearing audio "
                            "(%u ch @ %u Hz)\n", peak, 32767, g_mic_ch, g_mic_rate);
        else if (peak <= 2 && n > (uint64_t)g_mic_rate &&
                 !atomic_exchange_explicit(&said_quiet, 1, memory_order_relaxed))
            fprintf(stderr, "  [au] mic capture: %llu frames rendered but SILENT "
                            "(peak 0) — mic muted, no permission, or channel/format "
                            "mismatch\n", (unsigned long long)n);
    }

    // Append to the ring. The reader owns g_mic_r; we own g_mic_w. If the guest
    // is not draining fast enough the ring fills, and we drop the NEWEST frames
    // rather than touch g_mic_r from the wrong thread — a bounded, lock-free
    // overrun that a continuously-reading guest (the voice path) never reaches.
    size_t w = atomic_load_explicit(&g_mic_w, memory_order_relaxed);
    size_t r = atomic_load_explicit(&g_mic_r, memory_order_acquire);
    size_t freeframes = g_mic_cap - (w - r);
    size_t take = nframes;
    if (take > freeframes) {
        atomic_fetch_add_explicit(&g_mic_frames_drop, take - freeframes, memory_order_relaxed);
        take = freeframes;
    }
    for (size_t done = 0; done < take; ) {
        size_t off = (w + done) % g_mic_cap;
        size_t run = g_mic_cap - off;
        if (run > take - done) run = take - done;
        memcpy(g_mic_ring + off * g_mic_ch, g_cap_scratch + done * g_mic_ch,
               run * g_mic_ch * sizeof(int16_t));
        done += run;
    }
    atomic_store_explicit(&g_mic_w, w + take, memory_order_release);
    atomic_fetch_add_explicit(&g_mic_frames_cap, take, memory_order_relaxed);
    return noErr;
}

// The name of the capture unit actually in use (VoiceProcessingIO, or the
// RemoteIO/HALOutput fallback), for the one-time log.
static const char *g_mic_unit_name = "?";

static const char *mic_agc_name(void) {
    return g_mic_agc == 1 ? "on" : g_mic_agc == 0 ? "OFF" : "n/a";
}

// ---- the mic heartbeat ----
//
// The report's "microphone:" line is cumulative and rides on the output's live
// report; neither can say whether the mic CLIPS or how it is being clocked. So
// while a capture unit is open this prints one line every KL_MIC_REPORT_SECS
// (default 5, 0 = none): totals, the peak and the full-scale buffers since the
// line before, and the callback's frame size. Its own thread — started once, on
// the first open — so it touches none of the output side's state, and a run
// that never records never starts it.
static void mic_heartbeat(unsigned secs) {
    static uint64_t cap0, rd0, dr0, bufs0, clip0;
    uint64_t cap  = atomic_load(&g_mic_frames_cap);
    uint64_t rd   = atomic_load(&g_mic_frames_read);
    uint64_t dr   = atomic_load(&g_mic_frames_drop);
    uint64_t bufs = atomic_load(&g_mic_bufs);
    uint64_t clip = atomic_load(&g_mic_bufs_clip);
    int pk = atomic_exchange(&g_mic_peak_iv, 0);
    uint32_t lo = atomic_exchange(&g_mic_cb_min, 0), hi = atomic_exchange(&g_mic_cb_max, 0);
    double r = g_mic_rate ? (double)g_mic_rate : 48000.0;
    fprintf(stderr, "  [au] mic: %s AGC %s %u Hz/%u ch | %.2f s captured (+%.2f), "
                    "%.2f s read (+%.2f), %.2f s dropped (+%.2f) in %u s | "
                    "peak %d/32767 (%.1f dBFS), %llu of %llu buffers at full scale | "
                    "callback %u frames (%u..%u)%s\n",
            g_mic_unit_name, mic_agc_name(), g_mic_rate, g_mic_ch,
            cap / r, (cap - cap0) / r, rd / r, (rd - rd0) / r, dr / r, (dr - dr0) / r, secs,
            pk, pk ? 20.0 * log10((double)pk / 32767.0) : -99.0,
            (unsigned long long)(clip - clip0), (unsigned long long)(bufs - bufs0),
            atomic_load(&g_mic_cb_frames), lo, hi,
            hi > g_cap_scratch_frames && g_cap_scratch_frames ? " — CLIPPED TO THE SCRATCH" : "");
    cap0 = cap; rd0 = rd; dr0 = dr; bufs0 = bufs; clip0 = clip;
}

static void *mic_heartbeat_thread(void *unused) {
    (void)unused;
    pthread_setname_np("klepton-mic-heartbeat");
    unsigned secs = (unsigned)kl_env_int("KL_MIC_REPORT_SECS", 5);
    uint64_t last = 0;
    for (;;) {
        nap_ms(250);
        if (!g_mic_open) { last = 0; continue; }
        uint64_t now = now_ns();
        if (!last) { last = now; continue; }
        if (now - last < (uint64_t)secs * 1000000000ull) continue;
        last = now;
        mic_heartbeat(secs);
    }
    return NULL;
}

// Under g_mic_lock (kl_audio_mic_open), so it starts at most once.
static void mic_heartbeat_start(void) {
    static int started;
    if (started) return;
    started = 1;
    if (kl_env_int("KL_MIC_REPORT_SECS", 5) <= 0) return;
    pthread_t t;
    if (pthread_create(&t, NULL, mic_heartbeat_thread, NULL) != 0) {
        fprintf(stderr, "  [au] could not start the mic heartbeat thread\n");
        return;
    }
    pthread_detach(t);
}

// The client format — int16 interleaved at `rate`/`ch`, set on the OUTPUT scope
// of the input element (element 1). This is FORMAT PACKING ONLY: `rate`/`ch` are
// the mic's NATIVE values (see cap_build), so the unit converts neither rate nor
// channels — it just hands us signed 16-bit instead of its canonical float. The
// device->guest rate/channel conversion is kl_aaudio.c's job (anti-aliased there
// by its one-pole low-pass whenever the guest asks below the native rate).
static OSStatus cap_set_client_format(unsigned rate, unsigned ch) {
    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate       = rate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = ch;
    fmt.mFramesPerPacket  = 1;
    fmt.mBitsPerChannel   = 16;
    fmt.mBytesPerFrame    = (UInt32)(sizeof(int16_t) * ch);
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;
    return AudioUnitSetProperty(g_cap_unit, kAudioUnitProperty_StreamFormat,
                                kAudioUnitScope_Output, 1, &fmt, sizeof fmt);
}

// Build and initialise the capture unit with a given component subtype. Factored
// out so the VPIO -> RemoteIO fallback in cap_unit_create is one call each rather
// than a duplicated body. Captures the hardware's NATIVE rate + channel count —
// the input bus converts NEITHER, and a client format that disagrees initialises
// without error and then renders SILENCE (exactly how "capture the guest's 16 kHz
// mono" regressed Steam Link to no mic at all). kl_aaudio.c resamples and
// folds/fans-out to the guest's ask instead. Sets g_cap_unit / g_mic_rate /
// g_mic_ch / g_mic_dev_rate and, on success, g_mic_unit_name.
static int cap_build(OSType subtype, const char *name, int vpio) {
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = subtype;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) { fprintf(stderr, "  [au] no %s AudioComponent\n", name); return -1; }
    CHECK(AudioComponentInstanceNew(comp, &g_cap_unit), "AudioComponentInstanceNew(capture)");

    // Enable input (element 1), disable output (element 0): this instance only
    // records. VoiceProcessingIO has an output element too — its AEC reference —
    // but AEC is not the goal here (Steam Link does its own), so capture-only is
    // fine, and disabling output keeps this unit from also trying to drive the
    // speaker that our separate output unit (g_unit) owns.
    UInt32 one = 1, zero = 0;
    CHECK(AudioUnitSetProperty(g_cap_unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Input, 1, &one, sizeof one),
          "EnableIO(input,bus1)");
    CHECK(AudioUnitSetProperty(g_cap_unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Output, 0, &zero, sizeof zero),
          "DisableIO(output,bus0)");

#if TARGET_OS_OSX
    // HALOutput records from no device until told which; point it at the system
    // default input. (No-op on the visionOS units, which have exactly one.)
    AudioObjectPropertyAddress da = { kAudioHardwarePropertyDefaultInputDevice,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    AudioDeviceID dev = 0; UInt32 dsz = sizeof dev;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &dsz, &dev) == noErr && dev)
        CHECK(AudioUnitSetProperty(g_cap_unit, kAudioOutputUnitProperty_CurrentDevice,
                                   kAudioUnitScope_Global, 0, &dev, sizeof dev),
              "SetProperty(CurrentDevice)");
#endif

    // VoiceProcessingIO's voice-processing knobs, set defensively (a device that
    // rejects one still records) and only on VPIO — a plain RemoteIO/HALOutput has
    // no such properties. BypassVoiceProcessing = 0 makes sure the noise
    // suppression + AGC that cleans the raw beamformed array is actually ON;
    // EnableAGC = 1 asks for the automatic gain that lifts the low capture level.
    //
    // KL_MIC_AGC=0 (a Test switch in the start window) is the A/B for "the mic
    // sounds overdriven": the AGC is asked OFF instead. Asked, not merely left
    // alone — VPIO's own default for this property is on, so skipping the set
    // would change nothing. Default 1 is the behaviour this has always had.
    g_mic_agc = -1;
    if (vpio) {
        UInt32 off = 0, agc = kl_env_on("KL_MIC_AGC", 1) ? 1 : 0;
        OSStatus vp = AudioUnitSetProperty(g_cap_unit, kAUVoiceIOProperty_BypassVoiceProcessing,
                                           kAudioUnitScope_Global, 0, &off, sizeof off);
        OSStatus ag = AudioUnitSetProperty(g_cap_unit, kAUVoiceIOProperty_VoiceProcessingEnableAGC,
                                           kAudioUnitScope_Global, 0, &agc, sizeof agc);
        if (vp != noErr || ag != noErr) {
            char b1[8], b2[8];
            fprintf(stderr, "  [au] VPIO props (non-fatal): bypass=%s, AGC=%s\n",
                    vp == noErr ? "ok" : fourcc(vp, b1), ag == noErr ? "ok" : fourcc(ag, b2));
        }
    }

    double hw = measure_input_rate(g_cap_unit);
    if (hw <= 0.0) hw = g_session_rate > 0.0 ? g_session_rate : 48000.0;
    g_mic_dev_rate = (unsigned)hw;
    unsigned hw_ch = measure_input_channels(g_cap_unit);
    g_mic_ch = hw_ch < 1 ? 1 : (hw_ch > 2 ? 2 : hw_ch);
    g_mic_rate = g_mic_dev_rate;

    // The input callback is registered on the global scope; the element it names
    // is the input bus the callback then renders from.
    AURenderCallbackStruct cb = {capture_cb, NULL};
    CHECK(AudioUnitSetProperty(g_cap_unit, kAudioOutputUnitProperty_SetInputCallback,
                               kAudioUnitScope_Global, 0, &cb, sizeof cb),
          "SetProperty(SetInputCallback)");

    // Scratch big enough for any plausible burst; the callback clips to it.
    free(g_cap_scratch);
    g_cap_scratch_frames = 4096;
    g_cap_scratch = calloc(g_cap_scratch_frames * g_mic_ch, sizeof(int16_t));
    if (!g_cap_scratch) { fprintf(stderr, "  [au] capture scratch OOM\n"); goto fail; }

    // Native format — format packing only (int16 interleaved at the device rate
    // and native channel count); no rate or channel conversion in the unit.
    CHECK(cap_set_client_format(g_mic_rate, g_mic_ch), "SetProperty(StreamFormat,capture)");
    CHECK(AudioUnitInitialize(g_cap_unit), "AudioUnitInitialize(capture)");

    // What the unit says the AGC is now, not what we asked: the one-time line
    // and every heartbeat carry it, so an A/B log cannot be misread.
    if (vpio) {
        UInt32 v = 0, vsz = sizeof v;
        if (AudioUnitGetProperty(g_cap_unit, kAUVoiceIOProperty_VoiceProcessingEnableAGC,
                                 kAudioUnitScope_Global, 0, &v, &vsz) == noErr)
            g_mic_agc = v ? 1 : 0;
    }

    g_mic_unit_name = name;
    return 0;
fail:
    if (g_cap_unit) { AudioComponentInstanceDispose(g_cap_unit); g_cap_unit = NULL; }
    free(g_cap_scratch); g_cap_scratch = NULL; g_cap_scratch_frames = 0;
    return -1;
}

// ---- the capture unit ----
static int cap_unit_create(unsigned want_rate, unsigned want_ch) {
    (void)want_rate; (void)want_ch;   // captured native; kl_aaudio.c converts to the guest's ask
#if TARGET_OS_OSX
    // macOS is the `make check` host, not the target: HALOutput records raw, which
    // is enough to verify the plumbing. No VPIO fallback dance here.
    if (cap_build(kAudioUnitSubType_HALOutput, "HALOutput", 0) != 0) return -1;
#else
    // VoiceProcessingIO is Apple's mic-capture unit: it noise-suppresses + AGCs
    // the raw beamformed Vision Pro array (which sounds metallic/tunnelly and
    // captured low, ~759/32767, through a plain RemoteIO) and it OWNS the mic, so
    // it coexists with the .voiceChat session instead of being starved by it. If
    // VPIO will not initialise, fall back to plain RemoteIO — "works but raw"
    // rather than silent.
    //
    // KL_MIC_RAW=1 (a Test switch in the start window) goes straight to that
    // RemoteIO: no noise suppression, no AGC, no voice processing at all — the
    // other half of the "overdriven / crackling" A/B. Off by default.
    if (kl_env_on("KL_MIC_RAW", 0)) {
        fprintf(stderr, "  [au] KL_MIC_RAW=1 — capturing through plain RemoteIO (raw mic)\n");
        if (cap_build(kAudioUnitSubType_RemoteIO, "RemoteIO (KL_MIC_RAW)", 0) != 0) return -1;
    } else if (cap_build(kAudioUnitSubType_VoiceProcessingIO, "VoiceProcessingIO", 1) != 0) {
        fprintf(stderr, "  [au] VPIO capture unavailable — falling back to plain "
                        "RemoteIO (raw mic)\n");
        if (cap_build(kAudioUnitSubType_RemoteIO, "RemoteIO", 0) != 0) return -1;
    }
#endif

    // The one-time verdict the next steamlink log is read against.
    fprintf(stderr, "  [au] microphone capture: %s, AGC %s, native %u Hz/%u ch int16 (device %u Hz)\n",
            g_mic_unit_name, mic_agc_name(), g_mic_rate, g_mic_ch, g_mic_dev_rate);
    return 0;
}

// ---- the opt-in surface (see kl_audio.h) ----
void kl_audio_mic_set_enabled(int on) {
    on = on ? 1 : 0;
    if (on == g_mic_enabled) return;
    g_mic_enabled = on;
    fprintf(stderr, "  [au] microphone capture %s\n", on ? "armed (lazy — opens on first guest input stream)" : "disarmed");
    // Turning it off closes the device and drops the ring immediately: the whole
    // point of the toggle is that OFF touches no hardware.
    if (!on) kl_audio_mic_close();
}

int kl_audio_mic_enabled(void) { return g_mic_enabled; }

int kl_audio_mic_open(unsigned want_rate, unsigned want_channels) {
    if (!g_mic_enabled) return -1;   // gated: no capture while the toggle is off
    if (!kl_env_on("KL_AUDIO", 1)) {
        static int said;
        if (!said++) fprintf(stderr, "  [au] KL_AUDIO=0 — no capture device\n");
        return -1;
    }
    pthread_mutex_lock(&g_mic_lock);
    // Idempotent: a second input stream reuses the unit already open. Steam Link
    // opens ONE input stream, so this is the common path; a later stream at a
    // different rate does not re-open the unit — it captures at the first rate and
    // lets kl_aaudio.c resample the difference (see klaa_capture_into).
    if (g_mic_open) { pthread_mutex_unlock(&g_mic_lock); return 0; }

    if (cap_unit_create(want_rate, want_channels) != 0) {
        pthread_mutex_unlock(&g_mic_lock);
        return -1;
    }

    // A one-second ring, like the output side, so a scheduling hiccup on either
    // side cannot wrap it. Latency here is set by how promptly the guest reads,
    // not by a fill target — a capture stream wants the freshest frames.
    free(g_mic_ring);
    g_mic_cap = (size_t)g_mic_rate + 1;
    g_mic_ring = calloc(g_mic_cap * g_mic_ch, sizeof(int16_t));
    if (!g_mic_ring) {
        if (g_cap_unit) { AudioComponentInstanceDispose(g_cap_unit); g_cap_unit = NULL; }
        free(g_cap_scratch); g_cap_scratch = NULL; g_cap_scratch_frames = 0;
        pthread_mutex_unlock(&g_mic_lock);
        return -1;
    }
    atomic_store(&g_mic_w, 0); atomic_store(&g_mic_r, 0);

    OSStatus s = AudioOutputUnitStart(g_cap_unit);
    if (s != noErr) {
        char b[8];
        fprintf(stderr, "  [au] AudioOutputUnitStart(capture) failed: %s\n", fourcc(s, b));
        AudioUnitUninitialize(g_cap_unit);
        AudioComponentInstanceDispose(g_cap_unit); g_cap_unit = NULL;
        free(g_mic_ring); g_mic_ring = NULL; g_mic_cap = 0;
        free(g_cap_scratch); g_cap_scratch = NULL; g_cap_scratch_frames = 0;
        pthread_mutex_unlock(&g_mic_lock);
        return -1;
    }
    g_mic_open = 1;
    mic_heartbeat_start();
    pthread_mutex_unlock(&g_mic_lock);

    fprintf(stderr, "  [au] CoreAudio in: guest wants %u Hz/%u ch -> capturing %u Hz/%u ch%s\n",
            want_rate, want_channels, g_mic_rate, g_mic_ch,
            want_rate && want_rate != g_mic_rate ? " (guest resamples on read)" : "");
    return 0;
}

void kl_audio_mic_close(void) {
    pthread_mutex_lock(&g_mic_lock);
    if (g_cap_unit) {
        // Stop first: AudioOutputUnitStop blocks until the input callback has
        // returned, so freeing the ring/scratch afterward cannot race it.
        AudioOutputUnitStop(g_cap_unit);
        AudioUnitUninitialize(g_cap_unit);
        AudioComponentInstanceDispose(g_cap_unit);
        g_cap_unit = NULL;
    }
    g_mic_open = 0;
    free(g_mic_ring);    g_mic_ring = NULL;    g_mic_cap = 0;
    free(g_cap_scratch); g_cap_scratch = NULL; g_cap_scratch_frames = 0;
    atomic_store(&g_mic_w, 0); atomic_store(&g_mic_r, 0);
    pthread_mutex_unlock(&g_mic_lock);
}

unsigned kl_audio_mic_rate(void)     { return g_mic_rate; }
unsigned kl_audio_mic_channels(void) { return g_mic_ch; }

int kl_audio_mic_read_i16(int16_t *buf, int frames) {
    if (!g_mic_open || !g_mic_enabled || !buf || frames <= 0 || !g_mic_ring) return 0;
    size_t r = atomic_load_explicit(&g_mic_r, memory_order_relaxed);
    size_t w = atomic_load_explicit(&g_mic_w, memory_order_acquire);
    size_t have = w - r;
    size_t take = have < (size_t)frames ? have : (size_t)frames;
    for (size_t done = 0; done < take; ) {
        size_t off = (r + done) % g_mic_cap;
        size_t run = g_mic_cap - off;
        if (run > take - done) run = take - done;
        memcpy(buf + done * g_mic_ch, g_mic_ring + off * g_mic_ch,
               run * g_mic_ch * sizeof(int16_t));
        done += run;
    }
    atomic_store_explicit(&g_mic_r, r + take, memory_order_release);
    atomic_fetch_add_explicit(&g_mic_frames_read, take, memory_order_relaxed);
    return (int)take;
}
