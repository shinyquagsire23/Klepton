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
// calling, and the thing we call is guest code — kl_thread_init() first
// or the stack-protector prologue reads an empty TSD slot and the guest dies
// far from here.
#include "kl_aaudio.h"
#include "kl_env.h"

#include <math.h>
#include <pthread.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread/qos.h>

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

// An effects session is a framework object other APIs attach EQs and reverbs
// to. ALLOCATE asks the framework for one; NONE is "this stream has none".
enum { AAUDIO_SESSION_ID_NONE = -1, AAUDIO_SESSION_ID_ALLOCATE = 0 };

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
    // The attribute hints: usage, content type, capture policy, session id.
    // Nothing on this host acts on them: kl_audio.c opens one CoreAudio output
    // unit and macOS has no usage/content-type routing policy, no capture
    // policy and no effects session to attach to. They are recorded so the
    // stream getters answer what was ASKED rather than a zero the guest never
    // set — a guest that sets USAGE_GAME and reads back USAGE_MEDIA has been
    // told something false about its own stream. See klaa_setUsage.
    int32_t usage, content_type, capture_policy, session_id, privacy_sensitive;
    klaa_data_cb  data_cb;   void *data_user;
    klaa_error_cb error_cb;  void *error_user;
} klaa_builder;

typedef struct klaa_stream {
    int32_t rate, channels, format, burst;
    // What the BUILDER asked for, kept beside what openStream adopted. A log
    // that prints only the adopted format cannot answer whether corrupt-sounding
    // audio is a float/int16 mismatch: "int16" could equally have meant "the guest asked for int16"
    // or "the guest asked for nothing and we chose". Those are different
    // stories and only one of them is safe.
    int32_t req_format, req_rate, req_channels;
    // The bounded PCM capture — the guest end of the path. See kl_audio.h.
    // `cap_done` is set on the ONE attempt to open it, so an unwritable path
    // costs a single message rather than one per callback for the whole run.
    kl_wav *cap;
    int     cap_done;
    int32_t buffer_size;         // the guest's requested size, <= capacity
    // Carried from the builder so the getters can answer what was ADOPTED
    // rather than what was asked for. openStream substitutes for anything left
    // unspecified, and the substitution is the value in force — a getter that
    // replayed the builder's AAUDIO_UNSPECIFIED would describe a stream that
    // does not exist.
    int32_t direction, sharing_mode, performance_mode, input_preset, device_id;
    int32_t usage, content_type, session_id, capture_policy, privacy_sensitive;
    klaa_data_cb  data_cb;   void *data_user;
    klaa_error_cb error_cb;  void *error_user;

    pthread_mutex_t lock;
    int32_t state;
    int64_t frames_read_seen;          // getFramesRead's last answer: monotonic
    int      running;            // the feeder's own loop flag
    pthread_t thread;
    int      thread_live;

    void    *scratch;            // what the guest's callback fills, and what
                                 // kl_audio is handed verbatim
    size_t   scratch_bytes;

    // Capture path (direction == INPUT only). The mic delivers interleaved int16
    // at kl_audio_mic_rate()/kl_audio_mic_channels(); these carry the fractional
    // resample position and any mic frames pulled-but-not-yet-consumed across
    // reads, so a rate ratio != 1 does not click at every call boundary.
    int16_t *in_hold;            // mic frames pulled but not fully consumed
    size_t   in_hold_cap;        // capacity of in_hold, in mic frames
    size_t   in_have;            // valid mic frames currently in in_hold
    double   in_pos;             // fractional read position within in_hold
    unsigned mic_rate, mic_ch;   // the capture format this stream resamples FROM
    // Anti-alias safety net for the fallback path only (mic_rate > rate, i.e. a
    // real downsample this code has to do because RemoteIO would not capture at
    // the guest rate). One-pole low-pass state per channel; unused when the
    // capture already arrives at the guest rate (ratio == 1, the normal case).
    float    in_lp[8];

    unsigned long callbacks, frames;   // for the report

    // ---- why a callback came back empty ----
    //
    // On Tetris Effect (Wwise on AAudio), of every 3038 buffers the owner
    // handed the sink exactly 506 were all-zero, in every 15 s census, for the
    // whole run — 1 in 6, dead constant, while the music played at 0 dBFS.
    // That is 33 five-millisecond holes per second, heard as constant stutter.
    // The sink's own numbers were clean: no underruns after ownership, no short
    // writes, no restarts, playback advancing in real time. So the holes are
    // not in our output path — the guest's callback is HANDED a zeroed buffer
    // and gives it straight back.
    //
    // Two things do that and they need different fixes, so measure which:
    //
    //   STARVED  — the guest's own render thread missed its deadline and its
    //              sink had nothing to copy. Zeros clump, and the callback
    //              returns fast (it is a memcpy of nothing). Fix is scheduling.
    //   MISFIT   — the callback size and the guest's mixer block do not divide,
    //              so every Nth callback falls in a gap. Zeros are PERIODIC:
    //              the run-length histogram collapses onto one bucket and the
    //              gap between zeros is constant. Fix is the burst size.
    //
    // A run-length histogram of consecutive zero buffers plus the callback's
    // own duration separates them in one census.
    unsigned long zero_cbs;            // callbacks that returned all-zero
    unsigned long zero_runs[6];        // run lengths 1,2,3,4,5,6+
    unsigned long cur_zero_run;
    unsigned long gap_sum, gap_n;      // lit buffers between zero runs
    unsigned long cur_gap;
    double        cb_time, cb_time_zero;   // seconds inside the guest callback
    unsigned long cb_n, cb_n_zero;

    // ---- the PARTIAL fill, which is the misfit's actual damage ----
    //
    // An all-zero count never asks whether a buffer came back HALF full, and
    // that is the case a guest-side capture answered: of 4000 written buffers,
    // 3140 carried 240 lit frames and 796 carried exactly 64 lit frames followed
    // by 176 of the feeder's own memset — one every fifth buffer. 4 x 240 + 64
    // = 1024, so the guest's mixer block is 1024 frames and a 240-frame burst
    // did not divide it; the leftover was written to the speaker as silence.
    // 176 zeros every 1200 frames is a 40 Hz gate on the music, and it costs
    // playing time too — the guest's audio runs ~15% behind the wall clock.
    //
    // Every other counter in this file is clean while that happens, because a
    // partly-filled buffer is lit, is not a hole, is not an underrun and is not
    // a seam. So it is counted here, and the block size is derived from the
    // fill rather than from the spacing of holes — the same number, reached
    // without needing a hole to exist at all.
    unsigned long lit_bufs;            // buffers that carried any signal
    unsigned long part_bufs;           // ...of which these had a zero TAIL
    unsigned long part_lit_frames;     // frames the guest wrote in those
    unsigned long part_zero_frames;    // frames it left for us to play as silence
    int32_t       part_len;            // the lit length they keep landing on
    unsigned long part_len_n;          // (Boyer-Moore majority, one int of state)
    unsigned long part_gap_sum, part_gap_n, part_cur_gap;

    // ---- and what we do about them: the block seam ----
    //
    // Differencing the 15 s censuses over four minutes on hardware:
    //
    //   writes 5087 -> 8127  = 3040 pulls, 506 all-zero    16.6%
    //   writes 8127 -> 11167 = 3040 pulls, 507 all-zero    16.7%
    //   ...                                                every census
    //   zero runs 1/2/3/4/5/6+ = 2039/0/0/0/0/2
    //   mean 5.0 lit buffers between runs
    //
    // EVERY completed run is exactly one buffer long, and they are exactly six
    // apart. That is not starvation — a late mixer produces runs of two, four,
    // nine. It is a block boundary: the guest's mixer hands back 5 x 240 = 1200
    // frames and then one empty buffer while it mixes the next block, forever.
    // The guest's callback costs the SAME 0.002 ms whether it produced audio or
    // silence, so it is a ring copy and never a mix — the mixing is on the
    // guest's own thread and the empty buffer is the copy finding the ring
    // momentarily dry.
    //
    // Playing that empty buffer costs more than the hole: per 1200-frame block
    // the sink is handed SIX bursts (1440 frames) where the guest composed five,
    // so the music also runs 20% behind the wall clock for as long as it plays.
    //
    // Dropping the empty buffer and re-pulling at once does not help: measured,
    // every sixth slot became a run of exactly THREE dry pulls and the OUTPUT
    // silent rate stayed at 16.7% to three figures. The mixer needs WALL TIME,
    // not more pulls.
    //
    // So the feeder spends the SINK'S LEAD. kl_audio.c keeps 80 ms of audio
    // queued ahead of the device (kl_audio_lead_ms), and a seam costs a few
    // milliseconds — so the feeder WAITS for the guest's next block rather than
    // writing anything, and the device plays out of the lead while it waits.
    // Per block the sink then gets exactly the 1200 frames the guest composed,
    // in 25 ms of wall time, which is real time with no hole in it.
    //
    // Three things bound the wait, because a wait that is wrong is an underrun:
    //
    //   * the LEAD. Never wait when kl_audio_lead_ms is below the floor — that
    //     is the only reading that says whether there is anything to spend.
    //   * the BUDGET. A seam resolves in well under one burst (every measured
    //     run was exactly one buffer, i.e. <5 ms); a wait that outlasts the
    //     budget is not a seam and the buffer is written.
    //   * RECENT AUDIO. A genuinely silent stream — a boot window of thousands
    //     of dry callbacks, a muted game — has no seam to wait for, and waiting
    //     through it would drain the lead for nothing. No lit buffer in the
    //     last `KLAA_SEAM_MEMORY_S` means no wait at all, and pacing is exactly
    //     what it was.
    unsigned long holes_dropped;       // empty buffers that never reached the sink
    unsigned long out_bufs, out_zero;  // what the sink actually got
    unsigned long seams_waited;        // seams we spent wall time on
    unsigned long seams_filled;        // ...that the guest then filled
    double        seam_wait_s;         // total wall time spent waiting
    double        seam_wait_worst_s;   // the longest single wait
    unsigned long seams_no_lead;       // refused: the sink had no lead to spend
    unsigned long seams_over_budget;   // waited the budget out and wrote silence
    double        last_lit_s;          // klaa_now() of the last lit pull
} klaa_stream;

// The device is a singleton (kl_audio.c owns one output unit), so a second
// simultaneous output stream would silently fight the first for it. Nothing
// measured does that; this counter is here so that if it ever happens the log
// says so rather than the sound going strange.
static int g_open_streams;      // OUTPUT streams sharing the one output device
static int g_input_streams;     // INPUT streams sharing the one capture device
static unsigned long g_short_writes;
static int g_input_refusals;    // input opens declined because the mic was off

// The live streams, so the census can read their counters. Registered at
// openStream and cleared at close; a slot that is NULL is simply skipped, so a
// missed deregistration costs a stale line and never a crash.
#define KLAA_MAX_STREAMS 4
static klaa_stream *g_streams[KLAA_MAX_STREAMS];
static pthread_mutex_t g_streams_lock = PTHREAD_MUTEX_INITIALIZER;

static void klaa_register(klaa_stream *s) {
    pthread_mutex_lock(&g_streams_lock);
    for (int i = 0; i < KLAA_MAX_STREAMS; i++)
        if (!g_streams[i]) { g_streams[i] = s; break; }
    pthread_mutex_unlock(&g_streams_lock);
}
static void klaa_deregister(klaa_stream *s) {
    pthread_mutex_lock(&g_streams_lock);
    for (int i = 0; i < KLAA_MAX_STREAMS; i++)
        if (g_streams[i] == s) g_streams[i] = NULL;
    pthread_mutex_unlock(&g_streams_lock);
}

static int klaa_burst_frames(void) {
    const char *e = getenv("KL_AAUDIO_BURST");
    int n = e ? atoi(e) : 0;
    // NOT "any sane value works", which is what this comment used to say. The
    // burst is the size of every pull the guest's mixer is asked to serve, and
    // a mixer that renders in fixed blocks serves it out of the block it has:
    // ask a 1024-frame Wwise block for 240 frames five times and the fifth pull
    // returns 64 lit frames and leaves 176 as the feeder's memset. Those zeros
    // are then played. A guest-side capture shows exactly that — 796 of
    // 4000 buffers filled to 64, one every fifth buffer, dead regular — and it
    // is a 40 Hz gate on the music, not a dropout. The full reading is in
    // kl_audio.h beside the constant.
    //
    // So the default is KL_AUDIO_NATIVE_FRAMES: a power of two, which divides
    // every block size a real mixer uses, and the same number the guest is told
    // through `AudioManager.getProperty(OUTPUT_FRAMES_PER_BUFFER)`. A guest
    // that wants a different size still says so with setFramesPerDataCallback
    // and is still obeyed; KL_AAUDIO_BURST=240 restores the misfit for an A/B.
    if (n < 32 || n > 8192) n = KL_AUDIO_NATIVE_FRAMES;
    return n;
}

// How many frames at the FRONT of a returned buffer the guest actually wrote.
//
// The feeder zeroes the whole burst before every pull, so a zero TAIL is the
// guest declining to fill it rather than audio that happens to be silent — and
// the difference between those two readings is the whole question. Scanning
// backwards costs one byte on a full buffer, which is the common case.
static int32_t klaa_lit_frames(const void *p, int32_t frames, int frame_bytes) {
    const unsigned char *b = p;
    size_t i = (size_t)frames * (size_t)frame_bytes;
    while (i && !b[i - 1]) i--;
    return (int32_t)((i + (size_t)frame_bytes - 1) / (size_t)frame_bytes);
}

// The seam knobs. All three default ON: a launch from the Home View carries no
// environment, so a fix that only exists when someone types it on a devicectl
// launch is not a fix for the build people actually run. The env vars exist to
// turn the fix OFF for an A/B and never to turn it on.
//
//   KL_AAUDIO_HOLE_DROP=0    one pull, and whatever it holds is written
//                            (the behaviour before seams were handled).
//   KL_AAUDIO_SEAM_WAIT_MS=0 drop and re-pull with no wait — the treadmill
//                            described on klaa_stream.
//
// The retry cap is now a safety rail rather than the mechanism — the wait
// budget is what actually bounds a seam — so it is generous. At 0.002 ms a pull
// the cost of a high cap is nil, and the cost of a low one is giving up on a
// seam that was one poll from resolving.
static int klaa_hole_drop(void) {
    const char *e = getenv("KL_AAUDIO_HOLE_DROP");
    if (!e || !*e) return 32;
    int n = atoi(e);
    if (n < 0) n = 0;
    if (n > 256) n = 256;
    return n;
}

// Wall time the feeder will spend waiting for one seam to fill. Measured:
// every completed zero run at exactly ONE buffer, i.e. the guest's mixer had
// the block ready inside 5 ms whenever it was given 5 ms — so 12 ms is more
// than twice what the measurement says is needed, and still a seventh of the
// lead it is spent out of.
static double klaa_seam_wait_ms(void) {
    const char *e = getenv("KL_AAUDIO_SEAM_WAIT_MS");
    if (!e || !*e) return 12.0;
    double v = atof(e);
    if (v < 0.0) v = 0.0;
    if (v > 200.0) v = 200.0;
    return v;
}

// ...and the lead below which no wait is affordable. kl_audio.c targets 80 ms
// and the render callback takes 10 ms at a time, so 30 ms is three callbacks of
// margin under the floor — the sink cannot reach empty through a wait that
// stops here.
static double klaa_seam_lead_floor_ms(void) {
    const char *e = getenv("KL_AAUDIO_SEAM_LEAD_MS");
    if (!e || !*e) return 30.0;
    double v = atof(e);
    if (v < 0.0) v = 0.0;
    return v;
}

// How recently the stream must have produced audio for a dry pull to count as
// a seam worth waiting for. One block (25 ms on Tetris Effect) plus slack.
#define KLAA_SEAM_MEMORY_S  0.100
// One poll of the wait, in nanoseconds. Short against the budget and long
// against a thread wake, so a seam costs a handful of naps and never a spin.
#define KLAA_SEAM_POLL_NS   1000000L

static const char *klaa_format_name(int32_t f) {
    switch (f) {
    case AAUDIO_FORMAT_INVALID:        return "INVALID";
    case AAUDIO_FORMAT_UNSPECIFIED:    return "UNSPECIFIED";
    case AAUDIO_FORMAT_PCM_I16:        return "PCM_I16";
    case AAUDIO_FORMAT_PCM_FLOAT:      return "PCM_FLOAT";
    case AAUDIO_FORMAT_PCM_I24_PACKED: return "PCM_I24_PACKED";
    case AAUDIO_FORMAT_PCM_I32:        return "PCM_I32";
    default:                           return "?";
    }
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

static double klaa_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Did this buffer carry anything? Cheap: the sink measures a real peak, this
// only has to answer the yes/no the histogram is keyed on, and it stops at the
// first non-zero byte.
static int klaa_any_signal(const void *p, size_t bytes) {
    const unsigned char *b = p;
    for (size_t i = 0; i < bytes; i++) if (b[i]) return 1;
    return 0;
}

static void *klaa_feeder(void *arg) {
    // Audio feeder at HIGH QoS. At default QoS this thread is throttled the
    // moment the compositor and guest saturate the P-cores, and every
    // scheduling gap longer than one burst is an audible skip (Steam Link) or
    // crackle (Wwise starvation). USER_INTERACTIVE is the class visionOS
    // schedules ahead of default work; the knob returns the old behaviour.
#ifdef __APPLE__
    if (kl_env_on("KL_AUDIO_QOS", 1))
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    klaa_stream *s = arg;
    kl_thread_init();                    // mandatory: the callback is guest code

    // This thread carries an audio deadline: it calls the guest's mixer and
    // then blocks for exactly one buffer's playback, so anything that preempts
    // it lands in the output as a hole. A plain pthread_attr is
    // QOS_CLASS_UNSPECIFIED — the legacy tier the scheduler may satisfy from an
    // efficiency core — which is the same argument klb_pthread_create now makes
    // for guest threads, and it applies harder here because there is no queue
    // in front of this one to absorb a late wake. USER_INTERACTIVE rather than
    // INITIATED: this IS a deadline thread, not "the user is waiting".
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    // One frame size, not two. kl_audio takes the guest's own encoding since
    // float landed there, so the staging copy this loop used to make is gone
    // and an "in" and an "out" size can no longer drift apart.
    const int frame_in = klaa_bytes_per_frame(s);
    const int hole_drop = klaa_hole_drop();
    const double seam_budget_s = klaa_seam_wait_ms() / 1000.0;
    const double seam_lead_ms  = klaa_seam_lead_floor_ms();

    for (;;) {
        pthread_mutex_lock(&s->lock);
        int running = s->running;
        pthread_mutex_unlock(&s->lock);
        if (!running) break;

        int32_t frames = s->burst;
        int32_t r = AAUDIO_CALLBACK_RESULT_CONTINUE;
        int lit = 0;

        // Pull until the guest gives us something. A dry pull between two lit
        // ones is the seam between two mixer blocks (see klaa_stream): it is
        // not audio, so it is not written — but re-pulling it immediately buys
        // nothing either, because what the mixer is short of is wall time. So
        // the seam is WAITED OUT of the sink's lead. See the knobs above for
        // the three bounds; `waited_s` accumulates across attempts and
        // `seam_open` records whether this slot was ever classified as a seam,
        // so the census can say how many resolved and how many did not.
        double waited_s = 0.0;
        int seam_open = 0;
        for (int attempt = 0; attempt <= hole_drop; attempt++) {
            memset(s->scratch, 0, (size_t)frames * frame_in);
            double t0 = klaa_now();
            r = s->data_cb ? s->data_cb(s, s->data_user, s->scratch, frames)
                           : AAUDIO_CALLBACK_RESULT_STOP;
            double took = klaa_now() - t0;
            s->callbacks++;

            // Classify the buffer BEFORE the format conversion, on the bytes
            // the guest actually wrote. This histogram measures the GUEST and
            // must keep doing so whether or not we drop what it hands back —
            // it is the only evidence that the hole is still there, and a
            // census that counted only what we wrote would report the defect
            // cured the moment we started hiding it.
            lit = klaa_any_signal(s->scratch, (size_t)frames * frame_in);
            s->cb_time += took; s->cb_n++;
            if (!lit) {
                s->cb_time_zero += took; s->cb_n_zero++;
                s->zero_cbs++;
                if (s->cur_zero_run == 0 && s->cur_gap) {
                    s->gap_sum += s->cur_gap; s->gap_n++; s->cur_gap = 0;
                }
                s->cur_zero_run++;
            } else {
                if (s->cur_zero_run) {
                    unsigned long b = s->cur_zero_run - 1;
                    if (b > 5) b = 5;
                    s->zero_runs[b]++;
                    s->cur_zero_run = 0;
                }
                s->cur_gap++;
                // ...and HOW MUCH of it the guest filled. See the struct.
                s->lit_bufs++;
                int32_t lf = klaa_lit_frames(s->scratch, frames, frame_in);
                // A tail has to be long enough to MEAN something. Audio that
                // genuinely crosses zero on the last frame of a buffer is
                // indistinguishable from one frame the guest did not fill, and
                // counting it would report a fault on a clean stream — a 997 Hz
                // tone does it about once every 170 buffers. A sixteenth of the
                // burst is far below the fault this counter exists to find (176
                // of 240) and far above anything a waveform does by accident.
                int32_t tail_min = frames >> 4;
                if (tail_min < 4) tail_min = 4;
                if (frames - lf >= tail_min) {
                    s->part_bufs++;
                    s->part_lit_frames  += (unsigned long)lf;
                    s->part_zero_frames += (unsigned long)(frames - lf);
                    if (s->part_len_n == 0)      { s->part_len = lf; s->part_len_n = 1; }
                    else if (lf == s->part_len)  { s->part_len_n++; }
                    else                         { s->part_len_n--; }
                    if (s->part_cur_gap) { s->part_gap_sum += s->part_cur_gap; s->part_gap_n++; }
                    s->part_cur_gap = 0;
                } else {
                    s->part_cur_gap++;
                }
            }

            if (lit) s->last_lit_s = t0 + took;
            if (lit || r == AAUDIO_CALLBACK_RESULT_STOP) break;
            if (attempt >= hole_drop) break;   // the safety rail; write what we have

            // Is this a seam, or is the stream simply silent? Only a stream
            // that was making audio a moment ago has a block on its way, and
            // waiting through a genuinely silent passage would drain the lead
            // for nothing at all — the boot window alone is 7218 dry pulls.
            if (seam_budget_s <= 0.0) { s->holes_dropped++; continue; }  // no-wait A/B
            if (s->last_lit_s <= 0.0 ||
                klaa_now() - s->last_lit_s > KLAA_SEAM_MEMORY_S) break;

            // ...and is there lead to spend? This is the reading that keeps a
            // wait from becoming an underrun: -1 is "no device, not playing, or
            // we are not the owner", and anything below the floor is a sink
            // that needs the buffer more than the guest needs the time.
            double lead = kl_audio_lead_ms(s);
            if (!(lead > seam_lead_ms)) {
                if (!seam_open) { s->seams_no_lead++; seam_open = 1; }
                break;
            }
            if (!seam_open) { s->seams_waited++; seam_open = 1; }
            if (waited_s >= seam_budget_s) { s->seams_over_budget++; break; }

            struct timespec nap = {0, KLAA_SEAM_POLL_NS};
            nanosleep(&nap, NULL);
            waited_s += (double)KLAA_SEAM_POLL_NS * 1e-9;
            s->holes_dropped++;
        }
        if (seam_open && waited_s > 0.0) {
            s->seam_wait_s += waited_s;
            if (waited_s > s->seam_wait_worst_s) s->seam_wait_worst_s = waited_s;
            if (lit) s->seams_filled++;
        }

        // Frames WRITTEN, which is the guest's own playback clock: a dropped
        // hole was never queued for playback, so counting it here would run
        // that clock fast by exactly the drop rate.
        s->frames += (unsigned long)frames;
        s->out_bufs++;
        if (!lit) s->out_zero++;

        // Straight handoff in either format. The float -> int16 squeeze that
        // used to happen here was a lossy step done twice (kl_audio's ring is
        // float anyway), and having two producers each own their own copy of
        // the conversion is what let kl_opensl.c go without one for so long.
        size_t bytes = (size_t)frames * (size_t)frame_in;
        // Tagged with this stream's pointer: the VR client runs two output
        // streams at once, and kl_audio mixes them per-source instead of
        // letting their feeder threads corrupt one ring.
        size_t played = kl_audio_write_src(s, s->scratch, bytes);

        // The GUEST end of the capture — see kl_audio.h. `pcm` is exactly what
        // the guest's callback produced, at the guest's own rate, before the
        // sink resamples, arbitrates or touches it. Opened on the first LIT
        // pull so the cap is spent on audio rather than on the boot window
        // (7,000 dry callbacks on Tetris Effect), and it closes itself.
        if (lit && !s->cap && !s->cap_done) {
            const char *path = getenv("KL_AAUDIO_DUMP");
            s->cap_done = 1;      // one attempt, whatever it costs
            if (path && *path) {
                s->cap = kl_wav_open(path, (unsigned)s->rate,
                                     (unsigned)s->channels,
                                     kl_audio_dump_seconds());
                if (s->cap)
                    fprintf(stderr, "  [aaudio]   ...that is the GUEST end of "
                                    "the path: the callback's own bytes, "
                                    "%s as delivered, before any resample or "
                                    "arbitration\n",
                            klaa_format_name(s->format));
            }
        }
        if (s->cap) {
            if (s->format == AAUDIO_FORMAT_PCM_FLOAT)
                kl_wav_write_f32(s->cap, (const float *)s->scratch, (size_t)frames);
            else if (s->format == AAUDIO_FORMAT_PCM_I16)
                kl_wav_write_s16(s->cap, (const int16_t *)s->scratch, (size_t)frames);
        }
        if (played < bytes) {
            // No device, KL_AUDIO=0, or one that stopped draining: pace the
            // remainder ourselves, which is byte-for-byte what this loop would
            // be with no audio at all. Same fallback as kl_opensl.c's feeder.
            g_short_writes++;
            uint64_t rem = ((uint64_t)bytes - played) / (uint64_t)frame_in;
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
        // An OUTPUT feeder can be parked inside kl_audio_write; pausing its
        // source is what lets it out so the join returns. An INPUT feeder only
        // ever sleep-polls the mic, so it exits on its own once running is clear.
        if (s->direction != AAUDIO_DIRECTION_INPUT) kl_audio_pause_src(s);
        pthread_join(s->thread, NULL);
    }
    pthread_mutex_lock(&s->lock);
    s->thread_live = 0;
    s->state = AAUDIO_STREAM_STATE_STOPPED;
    pthread_mutex_unlock(&s->lock);
}

// ---------------------------------------------------------------------------
// The capture path (direction == INPUT). The mirror of the feeder above with the
// arrow reversed: kl_audio.c's capture unit fills a ring with interleaved int16
// at the device rate, and this side pulls from it (kl_audio_mic_read_i16),
// resamples to the guest's rate, maps channels, and converts to the guest's
// format. The conversion lives here, on the consumer side, for the same reason
// the output conversion lives on the feeder: it keeps arithmetic off the
// real-time callback in kl_audio.c.

// A one-pole low-pass over mic frames [from, to), in place, per channel — the
// anti-alias pre-filter for the rare FALLBACK where the capture unit would not
// take the guest's rate and we must decimate in software (mic_rate > guest rate).
// The coefficient tracks the decimation depth (a ~= 1/ratio: a bigger ratio wants
// a lower cutoff); it is a gentle roll-off, not a brick wall, but it turns raw
// aliasing into something far less harsh. Never runs on the normal path, where
// the capture already arrives at the guest rate (ratio == 1).
static void klaa_lowpass(klaa_stream *s, size_t from, size_t to, unsigned mch, double ratio) {
    float a = (float)(1.0 / ratio);
    if (a <= 0.0f) a = 0.001f; else if (a > 1.0f) a = 1.0f;
    for (size_t i = from; i < to; i++)
        for (unsigned c = 0; c < mch; c++) {
            float x = (float)s->in_hold[i * mch + c];
            s->in_lp[c] += a * (x - s->in_lp[c]);
            s->in_hold[i * mch + c] = (int16_t)lrintf(s->in_lp[c]);
        }
}

// Produce up to `req` guest frames into `dst`, in the stream's format, from the
// microphone. Non-blocking when timeout_ns == 0; otherwise it sleep-polls until
// `req` frames are available or the deadline passes — the same bounded pacing
// the output feeder uses, and what AAudioStream_read's timeout means. Returns the
// number of guest frames actually produced.
static int32_t klaa_capture_into(klaa_stream *s, void *dst, int32_t req, int64_t timeout_ns) {
    if (!s || !dst || req <= 0) return 0;
    unsigned mch = s->mic_ch ? s->mic_ch : 1;
    unsigned gch = s->channels;
    // mic frames consumed per guest frame. mic_rate == guest rate (the common
    // case: both 48 kHz) makes this exactly 1.0 and the resampler a straight copy.
    double ratio = (double)(s->mic_rate ? s->mic_rate : s->rate) / (double)s->rate;

    // Grow in_hold to whatever this request could consume: req guest frames need
    // about req*ratio mic frames, plus one for the interpolation's right edge and
    // one already held as fractional carry.
    size_t need = (size_t)((double)req * ratio) + 2;
    if (need > s->in_hold_cap) {
        int16_t *p = realloc(s->in_hold, need * mch * sizeof(int16_t));
        if (!p) return 0;
        s->in_hold = p;
        s->in_hold_cap = need;
    }

    int64_t deadline = 0;
    struct timespec t0;
    if (timeout_ns > 0) { clock_gettime(CLOCK_MONOTONIC, &t0);
        deadline = (int64_t)t0.tv_sec * 1000000000LL + t0.tv_nsec + timeout_ns; }

    int16_t *out16 = (int16_t *)dst;
    float   *outf  = (float *)dst;
    int is_float = (s->format == AAUDIO_FORMAT_PCM_FLOAT);
    int32_t produced = 0;

    for (;;) {
        // Top up the held mic frames from the ring (non-blocking). in_have counts
        // frames still ahead of in_pos; keep at least the fractional right edge.
        if (s->in_have < need) {
            size_t prev = s->in_have;
            int got = kl_audio_mic_read_i16(s->in_hold + s->in_have * mch,
                                            (int)(need - s->in_have));
            if (got > 0) {
                s->in_have += (size_t)got;
                // Filter ONLY the newly-pulled frames, exactly once, so the carried
                // leftover from a previous call is never double-filtered. No-op
                // unless this is the downsample fallback (ratio > 1).
                if (ratio > 1.0) klaa_lowpass(s, prev, s->in_have, mch, ratio);
            }
        }

        // Resample/convert as many output frames as the held mic frames allow —
        // interpolation reads in_hold[i] and in_hold[i+1], so it needs i+1 in
        // range.
        while (produced < req) {
            double pos = s->in_pos;
            size_t i = (size_t)pos;
            if (i + 1 >= s->in_have) break;         // not enough source held yet
            float frac = (float)(pos - (double)i);
            for (unsigned c = 0; c < gch; c++) {
                unsigned sc = c < mch ? c : 0;      // mono fans out; extra ch -> ch0
                float a = (float)s->in_hold[i * mch + sc] * (1.0f / 32768.0f);
                float b = (float)s->in_hold[(i + 1) * mch + sc] * (1.0f / 32768.0f);
                float v = a + (b - a) * frac;
                if (is_float) {
                    outf[(size_t)produced * gch + c] = v;
                } else {
                    if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
                    out16[(size_t)produced * gch + c] = (int16_t)lrintf(v * 32767.0f);
                }
            }
            produced++;
            s->in_pos = pos + ratio;
        }

        // Consume the mic frames now fully behind the read position: shift the
        // remainder to the front so the next call continues seamlessly.
        size_t base = (size_t)s->in_pos;
        if (base > 0) {
            if (base > s->in_have) base = s->in_have;
            size_t rem = s->in_have - base;
            if (rem) memmove(s->in_hold, s->in_hold + base * mch, rem * mch * sizeof(int16_t));
            s->in_have = rem;
            s->in_pos -= (double)base;
        }

        if (produced >= req || timeout_ns <= 0) break;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((int64_t)now.tv_sec * 1000000000LL + now.tv_nsec >= deadline) break;
        usleep(2000);   // bounded poll, same 2 ms cadence as the output producer
    }

    s->frames += (unsigned long)produced;
    return produced;
}

// The capture feeder — only started for an INPUT stream that registered a data
// callback. AAudio's callback contract for input is the reverse of output: the
// buffer it hands the guest is already FILLED with captured audio, and the guest
// reads it. So this pulls a burst from the mic into scratch and calls the guest
// with it, pacing on the burst's own duration (there is no kl_audio_write to
// block on here). A stream that uses AAudioStream_read instead has no callback
// and no feeder; the read pulls on demand.
static void *klaa_input_feeder(void *arg) {
#ifdef __APPLE__
    if (kl_env_on("KL_AUDIO_QOS", 1))
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    klaa_stream *s = arg;
    kl_thread_init();                    // mandatory: the callback is guest code

    for (;;) {
        pthread_mutex_lock(&s->lock);
        int running = s->running;
        pthread_mutex_unlock(&s->lock);
        if (!running) break;

        int32_t frames = s->burst;
        // Fill scratch with captured audio in the guest's format. Bounded wait so
        // the loop still checks `running` about once per burst if the mic stalls.
        int64_t budget = (int64_t)frames * 1000000000LL / (s->rate ? s->rate : 48000);
        int32_t got = klaa_capture_into(s, s->scratch, frames, budget);
        if (got < frames) {
            // Under-run: zero the tail so the guest never sees stale samples.
            size_t off = (size_t)got * (size_t)klaa_bytes_per_frame(s);
            size_t rest = (size_t)(frames - got) * (size_t)klaa_bytes_per_frame(s);
            memset((char *)s->scratch + off, 0, rest);
        }
        s->callbacks++;

        int32_t r = s->data_cb ? s->data_cb(s, s->data_user, s->scratch, frames)
                               : AAUDIO_CALLBACK_RESULT_STOP;
        if (r == AAUDIO_CALLBACK_RESULT_STOP) {
            pthread_mutex_lock(&s->lock);
            s->running = 0;
            s->state = AAUDIO_STREAM_STATE_STOPPED;
            pthread_mutex_unlock(&s->lock);
            fprintf(stderr, "  [aaudio] input data callback returned STOP; stream stopped\n");
            break;
        }
    }
    return NULL;
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
// AAudioStreamBuilder_setChannelMask (API 32) — a channel LAYOUT mask instead of
// a plain count. ZIX's Wwise sets it and aborted on the missing entry point.
// Derive the channel count from the set-bit count so a builder that sets only
// the mask still opens with the right channels.
static void klaa_setChannelMask(klaa_builder *b, uint32_t mask) {
    if (!b || !mask) return;
    int c = __builtin_popcount(mask);
    if (c >= 1 && c <= 8) b->channels = c;
}
// Newer AAudio builder setters (API 31-32) that ZIX's Wwise sets. None change how
// we open the stream, so they are inert stores/no-ops — but they must EXIST, or
// the guest aborts on the missing entry point.
static void klaa_setSpatializationBehavior(klaa_builder *b, int32_t v)  { (void)b; (void)v; }
static void klaa_setIsContentSpatialized(klaa_builder *b, uint8_t v)    { (void)b; (void)v; }
static void klaa_setPackageName(klaa_builder *b, const char *v)         { (void)b; (void)v; }
static void klaa_setAttributionTag(klaa_builder *b, const char *v)      { (void)b; (void)v; }
static void klaa_setSampleRate(klaa_builder *b, int32_t v)       { if (b) b->rate = v; }
static void klaa_setBufferCapacityInFrames(klaa_builder *b, int32_t v) { if (b) b->buffer_capacity = v; }
static void klaa_setInputPreset(klaa_builder *b, int32_t v)      { if (b) b->input_preset = v; }
static void klaa_setDeviceId(klaa_builder *b, int32_t v)         { if (b) b->device_id = v; }
static void klaa_setUsage(klaa_builder *b, int32_t v)            { if (b) b->usage = v; }
static void klaa_setContentType(klaa_builder *b, int32_t v)      { if (b) b->content_type = v; }
static void klaa_setSessionId(klaa_builder *b, int32_t v)        { if (b) b->session_id = v; }
static void klaa_setAllowedCapturePolicy(klaa_builder *b, int32_t v) { if (b) b->capture_policy = v; }
static void klaa_setPrivacySensitive(klaa_builder *b, int32_t v) { if (b) b->privacy_sensitive = v; }


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

    // INPUT is opt-in now, not refused by design (see the header). While the mic
    // toggle is OFF it is still declined exactly as before — nothing touches the
    // hardware, and the guest sees the same "no capture device" it always has.
    // When it is ON, the branch below builds a real capture stream fed by
    // kl_audio.c's mic path.
    if (b->direction == AAUDIO_DIRECTION_INPUT && !kl_audio_mic_enabled()) {
        g_input_refusals++;
        fprintf(stderr, "  [aaudio] openStream(INPUT, preset %d) -> UNAVAILABLE: "
                        "microphone toggle is OFF (kl_aaudio.h)\n", b->input_preset);
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

    if (b->direction == AAUDIO_DIRECTION_INPUT) {
        // Open the capture device up front, so a permission denial or an
        // unavailable mic is visible here where the guest can still react — the
        // same "open at open, not at start" bargain the output path makes. A
        // voice-chat client asks for mono; we adopt what it requested and resample
        // the mic to it on read.
        if (kl_audio_mic_open((unsigned)rate, (unsigned)channels) != 0) {
            g_input_refusals++;
            fprintf(stderr, "  [aaudio] openStream(INPUT, preset %d): mic toggle on but "
                            "kl_audio_mic_open failed -> UNAVAILABLE\n", b->input_preset);
            return AAUDIO_ERROR_UNAVAILABLE;
        }

        klaa_stream *is = calloc(1, sizeof *is);
        if (!is) {
            if (g_input_streams == 0) kl_audio_mic_close();   // we just opened it
            return AAUDIO_ERROR_NO_MEMORY;
        }
        pthread_mutex_init(&is->lock, NULL);
        is->rate = rate; is->channels = channels; is->format = format;
        is->burst = b->frames_per_data_cb > 0 ? b->frames_per_data_cb : klaa_burst_frames();
        is->buffer_size = b->buffer_capacity > is->burst ? b->buffer_capacity : is->burst;
        is->data_cb = b->data_cb;   is->data_user = b->data_user;
        is->error_cb = b->error_cb; is->error_user = b->error_user;
        is->direction        = AAUDIO_DIRECTION_INPUT;
        is->sharing_mode     = b->sharing_mode;
        is->performance_mode = b->performance_mode;
        is->input_preset     = b->input_preset;
        is->device_id        = b->device_id;
        is->state = AAUDIO_STREAM_STATE_OPEN;
        // The capture format this stream resamples FROM. read/feeder use these to
        // map the device's frames onto the guest's requested rate/channels.
        is->mic_rate = kl_audio_mic_rate();
        is->mic_ch   = kl_audio_mic_channels();

        // scratch is the guest-format burst the callback feeder fills; staging is
        // unused on the input path but kept allocated so the getters and the free
        // in klaa_close need no direction special-casing.
        is->scratch_bytes = (size_t)is->burst * (size_t)klaa_bytes_per_frame(is);
        is->scratch = calloc(1, is->scratch_bytes ? is->scratch_bytes : 1);
        if (!is->scratch) {
            pthread_mutex_destroy(&is->lock);
            free(is);
            if (g_input_streams == 0) kl_audio_mic_close();
            return AAUDIO_ERROR_NO_MEMORY;
        }

        g_input_streams++;
        double ratio = is->rate ? (double)is->mic_rate / (double)is->rate : 1.0;
        fprintf(stderr, "  [aaudio] openStream(INPUT): guest %d Hz/%d ch/%s, burst %d "
                        "<- mic %u Hz/%u ch%s, resample ratio %.3f (%s)\n",
                rate, channels, format == AAUDIO_FORMAT_PCM_FLOAT ? "float" : "int16",
                is->burst, is->mic_rate, is->mic_ch, is->data_cb ? " (callback)" : " (read)",
                ratio,
                ratio > 1.0001 ? "downsample — low-pass engaged" :
                ratio < 0.9999 ? "upsample" : "1.0 — straight copy, no aliasing");
        *out = is;
        return AAUDIO_OK;
    }

    klaa_stream *s = calloc(1, sizeof *s);
    if (!s) return AAUDIO_ERROR_NO_MEMORY;
    pthread_mutex_init(&s->lock, NULL);
    s->rate = rate; s->channels = channels; s->format = format;
    s->req_format = b->format; s->req_rate = b->rate; s->req_channels = b->channels;
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
    s->usage            = b->usage;
    s->content_type     = b->content_type;
    s->capture_policy   = b->capture_policy;
    s->privacy_sensitive = b->privacy_sensitive;
    // The one routing attribute that is NOT read back as asked. SESSION_ID_ALLOCATE
    // (0) means "framework, give me an effects session"; there is no effects
    // framework here to give one, so the stream honestly has none (-1) rather
    // than reporting an id nothing can be attached to.
    s->session_id       = b->session_id == AAUDIO_SESSION_ID_ALLOCATE
                        ? AAUDIO_SESSION_ID_NONE : b->session_id;
    s->state = AAUDIO_STREAM_STATE_OPEN;

    s->scratch_bytes = (size_t)s->burst * (size_t)klaa_bytes_per_frame(s);
    s->scratch = calloc(1, s->scratch_bytes);
    if (!s->scratch) {
        free(s->scratch); free(s);
        return AAUDIO_ERROR_NO_MEMORY;
    }

    if (g_open_streams)
        fprintf(stderr, "  [aaudio] NOTE: a second output stream is open; the "
                        "device is a singleton and they will share it\n");
    g_open_streams++;
    klaa_register(s);
    kl_audio_reporter(kl_aaudio_report);   // ride along on the 15 s live report

    // Open the device now rather than at start, so a failure is visible where
    // the guest can still react to it. A failure is not fatal: the feeder
    // paces with usleep, which is the null-audio behaviour every earlier
    // measurement was taken against.
    kl_audio_open_src_fmt(s, "aaudio", (unsigned)rate, (unsigned)channels,
                          format == AAUDIO_FORMAT_PCM_FLOAT ? KL_AUDIO_F32 : KL_AUDIO_S16);

    fprintf(stderr, "  [aaudio] openStream: %d Hz, %d ch, %s, burst %d frames "
                    "(%s, from %s)%s\n",
            rate, channels,
            format == AAUDIO_FORMAT_PCM_FLOAT ? "float" : "int16", s->burst,
            (klaa_hole_drop() && klaa_seam_wait_ms() > 0.0)
                ? "block seams are waited out of the sink's lead"
                : klaa_hole_drop()
                    ? "KL_AAUDIO_SEAM_WAIT_MS=0, seams are dropped and re-pulled"
                    : "KL_AAUDIO_HOLE_DROP=0, every empty buffer is played",
            b->frames_per_data_cb > 0 ? "the guest's setFramesPerDataCallback"
                                      : "our default",
            kl_audio_active() ? "" : " (no device — pacing with usleep)");
    // ...and what was ASKED for, separately, because the line above reports
    // only what is in force. A guest that never called setFormat is one whose
    // mixer writes whatever IT chose into a buffer we sized from whatever WE
    // chose, and that is a sample-content corruption with no counter that can
    // see it. Printed always, so the answer is in every log rather than in the
    // one someone thought to instrument.
    fprintf(stderr, "  [aaudio]   the builder asked for: format %s, rate %s, "
                    "channels %s — %s\n",
            klaa_format_name(b->format), b->rate > 0 ? "set" : "UNSPECIFIED",
            b->channels > 0 ? "set" : "UNSPECIFIED",
            (b->format == AAUDIO_FORMAT_UNSPECIFIED ||
             b->format == AAUDIO_FORMAT_INVALID)
                ? "the guest left the FORMAT to us and will read it back with "
                  "AAudioStream_getFormat; we answer PCM_I16 and the feeder "
                  "copies int16, so the two agree only if the guest honours "
                  "that readback"
                : "the guest named the format, so there is nothing to guess");
    *out = s;
    return AAUDIO_OK;
}

// ---------------------------------------------------------------------------
// AAudioStream

static aaudio_result_t klaa_requestStart(klaa_stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;

    // INPUT: the capture unit is already running (opened at openStream). A stream
    // that registered a data callback gets a feeder that pulls the mic and hands
    // the guest each burst; a blocking-read stream (Steam Link) has no callback
    // and no feeder — AAudioStream_read pulls on demand — so starting only moves
    // it to STARTED.
    if (s->direction == AAUDIO_DIRECTION_INPUT) {
        pthread_mutex_lock(&s->lock);
        if (s->state == AAUDIO_STREAM_STATE_STARTED) { pthread_mutex_unlock(&s->lock); return AAUDIO_OK; }
        s->running = 1;
        s->state = AAUDIO_STREAM_STATE_STARTED;
        int want_feeder = s->data_cb && !s->thread_live;
        pthread_mutex_unlock(&s->lock);
        if (want_feeder) {
            if (pthread_create(&s->thread, NULL, klaa_input_feeder, s) == 0) {
                pthread_mutex_lock(&s->lock);
                s->thread_live = 1;
                pthread_mutex_unlock(&s->lock);
            } else {
                pthread_mutex_lock(&s->lock);
                s->running = 0;
                pthread_mutex_unlock(&s->lock);
                return AAUDIO_ERROR_INTERNAL;
            }
        }
        return AAUDIO_OK;
    }

    pthread_mutex_lock(&s->lock);
    if (s->thread_live) { pthread_mutex_unlock(&s->lock); return AAUDIO_OK; }
    s->running = 1;
    s->state = AAUDIO_STREAM_STATE_STARTING;
    // A blocking-write output stream (SDL's AAudio backend) has no data callback;
    // it pushes PCM through AAudioStream_write. No feeder to pull, so starting only
    // opens the device and moves to STARTED — the feeder is for callback streams.
    int want_feeder = s->data_cb != NULL;
    pthread_mutex_unlock(&s->lock);

    kl_audio_play_src(s);
    // A stream with NO data callback is the blocking-write shape: SDL2's
    // AAudio backend is the first guest of that kind here — it pushes mixer
    // blocks from its own audio thread via AAudioStream_write and there is
    // nothing for a feeder to pull. Spawning one anyway made the stream stop
    // itself at the first callback (the no-callback STOP answer), which is
    // why the write arrived at a stopped stream.
    if (!want_feeder) {
        pthread_mutex_lock(&s->lock);
        s->state = AAUDIO_STREAM_STATE_STARTED;
        pthread_mutex_unlock(&s->lock);
        return AAUDIO_OK;
    }
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

// The blocking-write half of the ABI. The feeder's shape inverted: the guest
// pushes, kl_audio_write is the same blocking drain either way, and a short
// write paces exactly as the feeder paces it — the null-audio fallback is
// byte-for-byte a device that drains at the nominal rate.
//
// The frame counter and the format conversion are shared with the feeder's
// accounting deliberately: getFramesWritten answers "what the device has
// consumed from the guest" on both shapes.
static int32_t klaa_write(klaa_stream *s, const void *buf, int32_t frames,
                          int64_t timeoutNanos) {
    (void)timeoutNanos;   // the write blocks on the device clock either way
    if (!s || !buf) return AAUDIO_ERROR_NULL;
    if (s->direction != AAUDIO_DIRECTION_OUTPUT) return AAUDIO_ERROR_INVALID_STATE;
    if (frames < 0) return AAUDIO_ERROR_OUT_OF_RANGE;
    pthread_mutex_lock(&s->lock);
    int started = s->running && s->state == AAUDIO_STREAM_STATE_STARTED;
    pthread_mutex_unlock(&s->lock);
    if (!started) return AAUDIO_ERROR_INVALID_STATE;
    if (!frames) return 0;

    const int frame_in  = klaa_bytes_per_frame(s);
    const unsigned char *src = buf;
    int32_t left = frames;
    while (left > 0) {
        // Chunked at one burst, so the pace of the writes is the pace a
        // callback-mode stream would have had. No staging copy: kl_audio takes
        // the guest's own encoding since float landed there, so the squeeze
        // that used to happen here is gone and cannot drift from the feeder's.
        int32_t chunk = left > s->burst ? s->burst : left;
        size_t bytes = (size_t)chunk * (size_t)frame_in;
        size_t played = kl_audio_write_src(s, src, bytes);
        if (played < bytes) {
            g_short_writes++;
            uint64_t rem = ((uint64_t)bytes - played) / (uint64_t)frame_in;
            useconds_t us = (useconds_t)(rem * 1000000ull / (uint64_t)s->rate);
            if (us) usleep(us > 100000 ? 100000 : us);
        }
        pthread_mutex_lock(&s->lock);
        s->frames += (unsigned long)chunk;
        pthread_mutex_unlock(&s->lock);
        src += (size_t)chunk * (size_t)frame_in;
        left -= chunk;
    }
    return frames;
}

static aaudio_result_t klaa_requestStop(klaa_stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    klaa_stop_join(s);
    // Only the OUTPUT side has a device ring to flush; the capture unit keeps
    // running for any other input stream and is torn down at close.
    if (s->direction != AAUDIO_DIRECTION_INPUT) kl_audio_flush_src(s);
    return AAUDIO_OK;
}

// Pause: the stream stops making progress and KEEPS what is queued (a stop
// flushes it; a pause does not), and requestStart resumes it. The feeder is
// parked and joined exactly as for a stop — a new one starts on resume — and
// the state reads PAUSED, which is what getState/waitForStateChange must see.
// SDL2's AAudio backend loads this with the rest of its function table and
// refuses the whole backend when it is missing (Half-Life: Alyx's SDL core
// self-test: "Couldn't load AAUDIO function AAudioStream_requestPause").
static aaudio_result_t klaa_requestPause(klaa_stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    klaa_stop_join(s);
    pthread_mutex_lock(&s->lock);
    s->state = AAUDIO_STREAM_STATE_PAUSED;
    pthread_mutex_unlock(&s->lock);
    return AAUDIO_OK;
}

// AAudioStream_read — the blocking-read capture path Steam Link uses. Pull up to
// numFrames guest frames from the mic; if fewer are available and timeoutNanos
// is positive, sleep-poll up to the timeout, matching how the output feeder
// paces. Returns the frame count actually read (never an error for a partial
// read — that is the ABI's contract for a timeout).
static aaudio_result_t klaa_read(klaa_stream *s, void *buffer, int32_t numFrames,
                                 int64_t timeoutNanos) {
    if (!s || !buffer) return AAUDIO_ERROR_NULL;
    if (s->direction != AAUDIO_DIRECTION_INPUT) return AAUDIO_ERROR_INVALID_STATE;
    if (numFrames <= 0) return 0;
    return klaa_capture_into(s, buffer, numFrames, timeoutNanos);
}

static aaudio_result_t klaa_close(klaa_stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    klaa_stop_join(s);
    fprintf(stderr, "  [aaudio] close(%s): %lu callbacks, %lu frames\n",
            s->direction == AAUDIO_DIRECTION_INPUT ? "input" : "output",
            s->callbacks, s->frames);
    if (s->direction == AAUDIO_DIRECTION_INPUT) {
        // The capture device is shared like the output one; close it only when
        // the last input stream is gone.
        if (--g_input_streams <= 0) { g_input_streams = 0; kl_audio_mic_close(); }
        free(s->in_hold);
    } else {
        if (--g_open_streams <= 0) g_open_streams = 0;
        klaa_deregister(s);
        kl_wav_close(s->cap); s->cap = NULL;
        kl_audio_close_src(s);
    }
    pthread_mutex_destroy(&s->lock);
    free(s->scratch); free(s);
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
// AAudioStream_getChannelMask (API 32) — the readback for setChannelMask. We
// store only the adopted channel COUNT, not the requested layout mask, so
// synthesize an index mask with that many low bits set: 1ch -> 0x1 (MONO),
// 2ch -> 0x3 (STEREO). popcount(mask) then equals getChannelCount(), keeping
// the two readbacks consistent — the same reason the other getters report the
// adopted stream. ZIX's Wwise reads this after opening and aborted on the
// missing entry point.
static uint32_t klaa_getChannelMask(klaa_stream *s) {
    if (!s || s->channels < 1 || s->channels > 8) return 0;
    return (uint32_t)((1u << s->channels) - 1u);
}
static int32_t klaa_getFormat(klaa_stream *s)          { return s ? s->format : 0; }
static int32_t klaa_getDirection(klaa_stream *s)       { return s ? s->direction : AAUDIO_DIRECTION_OUTPUT; }
static int32_t klaa_getSharingMode(klaa_stream *s)     { return s ? s->sharing_mode : 0; }
static int32_t klaa_getPerformanceMode(klaa_stream *s) { return s ? s->performance_mode : 0; }
static int32_t klaa_getInputPreset(klaa_stream *s)     { return s ? s->input_preset : 0; }
// AAudioStream_getSpatializationBehavior (API 32) — the readback for
// setSpatializationBehavior. That setter is a no-op because our CoreAudio sink
// mixes flat and never spatializes, so we report NEVER (2) consistently with
// getIsContentSpatialized == false rather than echoing a request we do not honor.
// enum: UNSPECIFIED=0, AUTO=1, NEVER=2.
static int32_t klaa_getSpatializationBehavior(klaa_stream *s) { (void)s; return 2; }

// The routing attributes, read back as adopted. See the builder for why they
// are recorded rather than acted on.
static int32_t klaa_getUsage(klaa_stream *s)           { return s ? s->usage : 0; }
static int32_t klaa_getContentType(klaa_stream *s)     { return s ? s->content_type : 0; }
static int32_t klaa_getSessionId(klaa_stream *s)       { return s ? s->session_id : AAUDIO_SESSION_ID_NONE; }
static int32_t klaa_getAllowedCapturePolicy(klaa_stream *s) { return s ? s->capture_policy : 0; }
static int32_t klaa_isPrivacySensitive(klaa_stream *s)  { return s ? s->privacy_sensitive : 0; }

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
    if (!s || s->direction == AAUDIO_DIRECTION_INPUT) return 0;   // input writes nothing
    int64_t n;
    pthread_mutex_lock(&s->lock);
    n = (int64_t)s->frames;
    pthread_mutex_unlock(&s->lock);
    return n;
}
// On an OUTPUT stream AAudio's framesRead is the DEVICE's side of the queue:
// frames the hardware has taken out of the stream's buffer (framesWritten is
// the app's side). It used to answer 0 on the reading "an output stream reads
// nothing", which is the wrong half of the ABI — and a caller that checks for
// real playback progress (SDL2's AAudio self-test, under Half-Life: Alyx:
// "AAudio real output progress: written=107776 read=0") saw a device that
// never played. The sink knows what is still queued for this source
// (kl_audio_lead_ms), so read = written - queued, never negative and never
// running backwards.
static int64_t klaa_getFramesRead(klaa_stream *s) {
    if (!s) return 0;
    if (s->direction == AAUDIO_DIRECTION_INPUT) {   // an input stream's counter IS its frames read
        pthread_mutex_lock(&s->lock);
        int64_t n = (int64_t)s->frames;
        pthread_mutex_unlock(&s->lock);
        return n;
    }
    pthread_mutex_lock(&s->lock);
    int64_t written = (int64_t)s->frames;
    int32_t rate = s->rate;
    pthread_mutex_unlock(&s->lock);
    double lead_ms = kl_audio_lead_ms(s);
    int64_t queued = lead_ms > 0 && rate > 0 ? (int64_t)(lead_ms * rate / 1000.0) : 0;
    int64_t read = written - queued;
    if (read < 0) read = 0;
    pthread_mutex_lock(&s->lock);
    if (read < s->frames_read_seen) read = s->frames_read_seen;
    s->frames_read_seen = read;
    pthread_mutex_unlock(&s->lock);
    return read;
}

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

// The audio session id, used to attach platform audio effects. There is no
// Android AudioSession here, so NONE (-1): no session to hang effects on, which
// is what an app with no effects requested would also see.

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
    // setSamplesPerFrame is the OLD NAME for setChannelCount, exactly as the
    // getter pair already is, and libUE4.so imports BOTH names — a guest that
    // dlsym'd this one and got NULL would be configuring a stream through a
    // door that is not there.
    A("AAudioStreamBuilder_setSamplesPerFrame",    klaa_setChannelCount),
    A("AAudioStreamBuilder_setChannelMask",        klaa_setChannelMask),
    A("AAudioStreamBuilder_setSpatializationBehavior", klaa_setSpatializationBehavior),
    A("AAudioStreamBuilder_setIsContentSpatialized",   klaa_setIsContentSpatialized),
    A("AAudioStreamBuilder_setPackageName",            klaa_setPackageName),
    A("AAudioStreamBuilder_setAttributionTag",         klaa_setAttributionTag),
    A("AAudioStreamBuilder_setSampleRate",         klaa_setSampleRate),
    A("AAudioStreamBuilder_setBufferCapacityInFrames", klaa_setBufferCapacityInFrames),
    A("AAudioStreamBuilder_setInputPreset",        klaa_setInputPreset),
    A("AAudioStreamBuilder_setDataCallback",       klaa_setDataCallback),
    A("AAudioStreamBuilder_setErrorCallback",      klaa_setErrorCallback),
    A("AAudioStreamBuilder_openStream",            klaa_openStream),
    A("AAudioStreamBuilder_delete",                klaa_builder_delete),
    A("AAudioStream_requestStart",                 klaa_requestStart),
    A("AAudioStream_requestStop",                  klaa_requestStop),
    A("AAudioStream_requestPause",                 klaa_requestPause),
    A("AAudioStream_read",                         klaa_read),
    A("AAudioStream_write",                        klaa_write),
    A("AAudioStream_close",                        klaa_close),
    A("AAudioStream_getFramesPerBurst",            klaa_getFramesPerBurst),
    A("AAudioStream_getFramesPerDataCallback",     klaa_getFramesPerDataCallback),
    A("AAudioStream_getSampleRate",                klaa_getSampleRate),
    A("AAudioStream_getChannelCount",              klaa_getChannelCount),
    A("AAudioStream_getChannelMask",               klaa_getChannelMask),
    A("AAudioStream_getSamplesPerFrame",           klaa_getSamplesPerFrame),
    A("AAudioStream_getFormat",                    klaa_getFormat),
    A("AAudioStream_getDirection",                 klaa_getDirection),
    A("AAudioStream_getSharingMode",               klaa_getSharingMode),
    A("AAudioStream_getPerformanceMode",           klaa_getPerformanceMode),
    A("AAudioStream_getInputPreset",               klaa_getInputPreset),
    A("AAudioStream_getSpatializationBehavior",    klaa_getSpatializationBehavior),
    A("AAudioStream_getState",                     klaa_getState),
    A("AAudioStream_getFramesWritten",             klaa_getFramesWritten),
    A("AAudioStream_getFramesRead",                klaa_getFramesRead),
    A("AAudioStreamBuilder_setFramesPerDataCallback", klaa_setFramesPerDataCallback),
    A("AAudioStreamBuilder_setDeviceId",           klaa_setDeviceId),
    A("AAudioStreamBuilder_setUsage",              klaa_setUsage),
    A("AAudioStreamBuilder_setContentType",        klaa_setContentType),
    A("AAudioStreamBuilder_setAllowedCapturePolicy", klaa_setAllowedCapturePolicy),
    A("AAudioStreamBuilder_setSessionId",          klaa_setSessionId),
    A("AAudioStreamBuilder_setPrivacySensitive",   klaa_setPrivacySensitive),
    A("AAudioStream_waitForStateChange",           klaa_waitForStateChange),
    A("AAudioStream_getBufferCapacityInFrames",    klaa_getBufferCapacityInFrames),
    A("AAudioStream_getBufferSizeInFrames",        klaa_getBufferSizeInFrames),
    A("AAudioStream_setBufferSizeInFrames",        klaa_setBufferSizeInFrames),
    A("AAudioStream_getDeviceId",                  klaa_getDeviceId),
    A("AAudioStream_getSessionId",                 klaa_getSessionId),
    A("AAudioStream_getXRunCount",                 klaa_getXRunCount),
    A("AAudioStream_isMMapUsed",                   klaa_isMMapUsed),
    A("AAudioStream_getUsage",                     klaa_getUsage),
    A("AAudioStream_getContentType",               klaa_getContentType),
    A("AAudioStream_getAllowedCapturePolicy",      klaa_getAllowedCapturePolicy),
    A("AAudioStream_isPrivacySensitive",           klaa_isPrivacySensitive),
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
static const char g_aa_handle[] = "klepton-aaudio";

// Every AAudio name we were asked for and did not serve, kept so the report can
// name them. Small and fixed: the loader below asks for about fifty, and a name
// asked for twice is the same fact twice.
enum { KLAA_DECLINED_MAX = 64 };
static const char *g_declined[KLAA_DECLINED_MAX];
static int         g_declined_n;

static void klaa_decline(const char *name) {
    for (int i = 0; i < g_declined_n; i++)
        if (!strcmp(g_declined[i], name)) return;
    if (g_declined_n < KLAA_DECLINED_MAX) {
        // dlsym's argument belongs to the caller; keep our own copy.
        char *copy = strdup(name);
        if (!copy) return;
        g_declined[g_declined_n++] = copy;
    }
    fprintf(stderr, "  [aaudio] no %s — answering NULL, as an absent Android "
                    "release does\n", name);
}

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

// dlsym on that handle, and it answers NULL for a name we do not serve — which
// is the OPPOSITE of what the import door does, and deliberately so.
//
// This used to hand back a named stub that aborted when called, on the standing
// rule that "we do not serve this" should fail by name rather than at address
// zero. That rule is right for the ELF-import door, where a NULL binding IS
// address zero and there is nobody left to ask. It is wrong here, because dlsym
// on libaaudio.so is not a binding — it is a QUESTION, and every caller that
// asks it already knows the answer may be no.
//
// Oboe (the Meta XR Audio sink's AAudio backend, and FMOD's) is built entirely
// on that question. AAudioLoader::open() dlsyms about fifty names into a table
// of function pointers and null-checks every one at the call site; roughly six
// are load-bearing and the rest are "use it if this Android release has it".
// Handing back a stub told Oboe that every one of them existed, so the first
// optional call went straight to the abort — Batman: Arkham Shadow died in
// AAudio_getMMapPolicy, which Oboe looks up precisely so it can find out
// whether the MMap policy knobs exist at all, and handles being told no.
//
// The six that AAudioLoader treats as mandatory — setChannelCount,
// setBufferCapacityInFrames, setSampleRate, AAudioStreamBuilder_delete,
// AAudioStream_getChannelCount, AAudioStream_close, convertResultToText — are
// all in the table above, verified against libMetaXRAudioWwise's own loader, so
// saying no to the rest costs the guest a feature it can do without and never
// the stream. A refusal still goes to the log by name, once, so a guest that
// degrades because of one is diagnosable without being fatal.
void *kl_aaudio_sym(const char *name) {
    if (!name) return NULL;
    void *fn = kl_aaudio_lookup(name);
    if (fn) return fn;
    if (strncmp(name, "AAudio", 6) != 0) return NULL;   // not ours to answer for
    klaa_decline(name);
    return NULL;
}

void kl_aaudio_report(FILE *f) {
    if (g_open_streams || g_input_streams || g_short_writes || g_input_refusals)
        fprintf(f, "  [aaudio] %d output + %d input stream(s) open, %lu short writes, "
                   "%d input refusal(s)\n",
                g_open_streams, g_input_streams, g_short_writes, g_input_refusals);

    // Where the holes come from. See the counters on klaa_stream: a run-length
    // histogram that is all in the "1" bucket with a constant gap between runs
    // is a burst/mixer-block MISFIT; runs that spread across the buckets are
    // STARVATION. The callback's own duration separates them again from the
    // other side — a starved sink returns nothing fast, so its zero callbacks
    // are much cheaper than its lit ones.
    pthread_mutex_lock(&g_streams_lock);
    for (int i = 0; i < KLAA_MAX_STREAMS; i++) {
        klaa_stream *s = g_streams[i];
        if (!s || !s->cb_n) continue;
        double lit_ms  = s->cb_n > s->cb_n_zero
            ? 1000.0 * (s->cb_time - s->cb_time_zero) / (double)(s->cb_n - s->cb_n_zero) : 0.0;
        double zero_ms = s->cb_n_zero
            ? 1000.0 * s->cb_time_zero / (double)s->cb_n_zero : 0.0;
        // The negotiated format, in every census. The whole feeder is a
        // memcpy whose correctness rests on these four numbers agreeing with
        // what the guest's mixer actually writes, so they are printed beside
        // the buffer counts they explain.
        fprintf(f, "  [aaudio] negotiated: %d Hz, %d ch, %s (%d bytes/frame) — "
                   "the builder asked for %s / %d Hz / %d ch (0 = unspecified, "
                   "i.e. ours to choose)%s\n",
                s->rate, s->channels, klaa_format_name(s->format),
                klaa_bytes_per_frame(s), klaa_format_name(s->req_format),
                s->req_rate, s->req_channels,
                s->cap ? "  [capturing]" : "");
        fprintf(f, "  [aaudio] burst %d fr (%.2f ms): %lu callback(s), %lu came "
                   "back ALL-ZERO (%.1f%%)\n",
                s->burst, s->rate ? 1000.0 * s->burst / s->rate : 0.0,
                s->cb_n, s->zero_cbs,
                100.0 * (double)s->zero_cbs / (double)s->cb_n);
        if (s->zero_cbs) {
            unsigned long longer = s->zero_runs[1] + s->zero_runs[2] +
                                   s->zero_runs[3] + s->zero_runs[4] + s->zero_runs[5];
            unsigned long runs = s->zero_runs[0] + longer;
            // A run only enters the histogram when a LIT buffer closes it, so a
            // stream that has produced nothing but silence has an open run and
            // an empty histogram. Saying "CLUMPED" off that would be reading a
            // verdict out of no evidence — which is exactly what a host run
            // looks like when the guest stays silent there.
            const char *verdict =
                runs == 0 ? "no completed run yet — the stream has not produced "
                            "a lit buffer to close one, so there is nothing to "
                            "classify"
                : s->zero_runs[0] > 8 * longer
                    ? "PERIODIC: single-buffer holes at a fixed spacing, which "
                      "is a burst/mixer-block misfit, not starvation"
                    : "CLUMPED: multi-buffer runs, which is the guest's mixer "
                      "missing its deadline";
            fprintf(f, "  [aaudio]   zero runs 1/2/3/4/5/6+ = %lu/%lu/%lu/%lu/%lu/%lu"
                       " (%lu in flight), mean %.1f lit buffers between runs — %s\n",
                    s->zero_runs[0], s->zero_runs[1], s->zero_runs[2],
                    s->zero_runs[3], s->zero_runs[4], s->zero_runs[5],
                    s->cur_zero_run,
                    s->gap_n ? (double)s->gap_sum / (double)s->gap_n : 0.0,
                    verdict);
            // The block size, MEASURED rather than assumed. A periodic
            // single-buffer hole every N lit pulls means the guest's mixer
            // hands back N bursts and then goes away to mix the next block, so
            // the block is N x burst frames. Only meaningful under PERIODIC —
            // under CLUMPED the gap is a scheduling artefact and this number
            // would be a fiction, so it is printed with the verdict beside it.
            double gap = s->gap_n ? (double)s->gap_sum / (double)s->gap_n : 0.0;
            if (runs && s->zero_runs[0] > 8 * longer && gap > 0.5)
                fprintf(f, "  [aaudio]   the guest's mixer block measures ~%.0f "
                           "frames (%.1f ms) — %.1f burst(s) of %d\n",
                        gap * s->burst,
                        s->rate ? 1000.0 * gap * s->burst / s->rate : 0.0,
                        gap, s->burst);
        }
        // The PARTIAL fill. This is the line the all-zero census misses: a
        // buffer with a lit head and a zero tail passes every other test in
        // this census — it is lit, it is not a hole, it costs no underrun and
        // opens no seam — and it is the defect. Printed whenever it happens at
        // all, because "none" is the whole of the fix and has to be visible.
        if (s->part_bufs) {
            double gap   = s->part_gap_n
                         ? (double)s->part_gap_sum / (double)s->part_gap_n : 0.0;
            double block = gap * (double)s->burst + (double)s->part_len;
            double period = (gap + 1.0) * (double)s->burst;
            unsigned long tot = s->part_lit_frames + s->part_zero_frames
                              + (s->lit_bufs - s->part_bufs) * (unsigned long)s->burst;
            fprintf(f, "  [aaudio]   PARTIAL FILLS: %lu of %lu lit buffer(s) (%.1f%%) "
                       "came back with only %d lit frame(s) and %d ZERO — %lu frame(s) "
                       "of our own memset played as audio (%.1f%% of everything lit), "
                       "one such buffer every %.1f full one(s)\n",
                    s->part_bufs, s->lit_bufs,
                    s->lit_bufs ? 100.0 * (double)s->part_bufs / (double)s->lit_bufs : 0.0,
                    s->part_len, s->burst - s->part_len,
                    s->part_zero_frames,
                    tot ? 100.0 * (double)s->part_zero_frames / (double)tot : 0.0,
                    gap);
            // The block, DERIVED — but only when the fill length is actually
            // steady. `part_len` is a majority survivor, not a count, so it is
            // a real modal length only while its support is a majority of the
            // partial buffers; a mixer whose short fills land all over the
            // place has no single block to name and would get a fiction here.
            if (gap > 0.5 && s->rate && s->part_len_n * 2 > s->part_bufs)
                fprintf(f, "  [aaudio]     -> the guest's mixer block is ~%.0f frames "
                           "and the burst (%d) does not divide it; the zero tail "
                           "recurs every %.0f frames, which is a %.0f Hz GATE on the "
                           "music rather than silence. A burst that divides the block "
                           "(KL_AAUDIO_BURST) is the fix.\n",
                        block, s->burst, period, (double)s->rate / period);
        } else if (s->lit_bufs) {
            fprintf(f, "  [aaudio]   partial fills: none — every one of %lu lit "
                       "buffer(s) was filled to all %d frames (the burst divides the "
                       "guest's mixer block)\n", s->lit_bufs, s->burst);
        }
        fprintf(f, "  [aaudio]   guest callback took %.3f ms when it produced "
                   "audio, %.3f ms when it produced silence\n", lit_ms, zero_ms);
        // ...and what the SINK got, which is the number the ear is on. The two
        // percentages above and below are the whole point of the fix: the
        // guest's rate says the hole is still being produced, ours says it is
        // no longer being played.
        fprintf(f, "  [aaudio]   OUTPUT: %lu buffer(s) written, %lu of them "
                   "silent (%.1f%%), %lu hole(s) dropped (drop cap %d)\n",
                s->out_bufs, s->out_zero,
                s->out_bufs ? 100.0 * (double)s->out_zero / (double)s->out_bufs : 0.0,
                s->holes_dropped, klaa_hole_drop());
        // ...and how the seam was paid for. Dropping and re-pulling alone did
        // not move the OUTPUT percentage, because the mixer wanted wall time
        // and got pulls; this line is what says whether the wait is being
        // taken, whether it WORKS, and which bound stops it when it does not.
        // `resolved` near `waited` with a mean well inside the budget is the
        // fix engaged; a large `no lead` is a sink that never built one, and a
        // large `over budget` is a producer that is genuinely starved rather
        // than seamed — a different defect, and one this cannot fix.
        if (s->seams_waited || s->seams_no_lead)
            fprintf(f, "  [aaudio]   seams: %lu waited (%lu resolved, mean %.2f ms, "
                       "worst %.2f ms), %lu over budget (%.0f ms), %lu refused "
                       "for lead (< %.0f ms)\n",
                    s->seams_waited, s->seams_filled,
                    s->seams_waited ? 1000.0 * s->seam_wait_s / (double)s->seams_waited : 0.0,
                    1000.0 * s->seam_wait_worst_s,
                    s->seams_over_budget, klaa_seam_wait_ms(),
                    s->seams_no_lead, klaa_seam_lead_floor_ms());
    }
    pthread_mutex_unlock(&g_streams_lock);

    if (g_declined_n) {
        fprintf(f, "  [aaudio] %d name(s) asked for and not served:\n", g_declined_n);
        for (int i = 0; i < g_declined_n; i++)
            fprintf(f, "    %s\n", g_declined[i]);
    }
}
