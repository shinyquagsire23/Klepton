// t_aaudio — the block-seam harness.
//
// The AAudio defects this covers were read out of device logs, and could not be
// reproduced on the host because Tetris Effect's mixer stays silent there
// (m_boot reaches the title screen with `peak 0` after four minutes). So the
// GUEST is modelled here instead, from the facts the device logs measured about
// it and nothing else:
//
//   1. It is block-quantised: a 1024-frame mixer block (Wwise's refill block),
//      served out of its own ring.
//   2. Its callback costs the same 0.002 ms whether it produced audio or
//      silence, so the callback is a ring COPY and the mixing is on a thread of
//      the guest's own. That thread needs WALL TIME: two extra pulls issued
//      back to back produce nothing and the output silent rate does not move.
//
// The model is therefore a copy-callback in front of an asynchronous producer:
// when the guest's ring runs dry the callback asks for the next block and
// returns what it has, and the block lands KLAA_MIX_MS later. That is the
// whole mechanism, and it reproduces the device's seam numbers (see the
// PERIODIC assertion below) without needing the title.
//
// ---- the signal is a TONE, and the capture is under test ----
//
// Timing alone is not enough: with the seam wait engaged on hardware (2043
// seams waited, all resolved, 0.0% of the owned output silent, real-time
// pacing, zero underruns) the sound can still be wrong. Every remaining
// explanation is about the SAMPLES rather than about when they arrive:
// channels swapped, an interleave misread, a byte order, a format the guest
// and the feeder disagree about. None of those is visible to a counter and all
// of them are audible.
//
// So this harness generates a known SINE — different amplitude per channel, so
// the two are distinguishable — and asserts on what came out the far end of
// the capture, which is the same instrument the device writes. A pipeline that
// swaps L and R, reads int16 as float, or loses a channel to the interleave
// fails here rather than in a headset.
//
// The amplitude is 64 LSB (-54 dBFS): inaudible on the machine somebody is
// working at, and exactly measurable, because a digital sine's zero crossings
// are noise-free at any amplitude above a handful of LSBs. It also has to be
// non-zero at all — silence never takes ownership of the sink, and without
// ownership there is no lead to spend and nothing under test.
//
//   make aaudio
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kl_aaudio.h"
#include "kl_audio.h"

// ---- the partial fill ----
//
// A guest that serves all-or-nothing (the whole burst or zeros) with a block
// that is an exact multiple of the burst never has a leftover to mishandle, and
// cannot reproduce the real defect. A guest-side capture read off the device:
// of 4000 written buffers, 3140 carried 240 lit frames and 796 carried exactly
// 64 lit frames followed by 176 zeros — one every fifth buffer. 4 x 240 + 64 =
// 1024. The block is 1024 frames, the guest serves a PARTIAL buffer when that
// is all it has, and the feeder's own memset goes to the speaker as the rest.
// 176 zeros every 1200 frames is a 40 Hz gate on the music, and it also costs
// 15% of the playing time — the half the pitch assertion below catches.
//
// So the model serves min(available, frames), the block is 1024, and the
// assertion is that NO buffer comes back partly filled. At a burst of 240 that
// fails; at KL_AUDIO_NATIVE_FRAMES it passes, because 256 divides 1024.
enum { RATE = 48000, CH = 2, BLOCK = 1024 };

// How long the modelled mixer takes to produce a block once asked. Well inside
// the feeder's 12 ms seam budget, and deliberately longer than one burst (5 ms) so a fix
// that merely re-pulls cannot pass this test by accident.
static double g_mix_ms = 6.0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static long   g_avail;         // frames the guest's own ring holds
static int    g_asked;         // the callback has asked for the next block
static int    g_stop;
static long   g_phase;         // the tone's sample index, carried across blocks

// The tone. 997 Hz rather than 1000 so its period never divides the burst, the
// block or the second — a signal that lines up with the plumbing can be cut and
// respliced at a boundary with nothing to show for it.
static double g_tone_hz  = 997.0;
static int    g_tone_amp = 64;      // -54 dBFS. Right channel is half of this.

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void *mixer(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_asked && !g_stop) pthread_cond_wait(&g_cond, &g_lock);
        if (g_stop) { pthread_mutex_unlock(&g_lock); break; }
        g_asked = 0;
        pthread_mutex_unlock(&g_lock);

        struct timespec nap = {0, (long)(g_mix_ms * 1e6)};
        nanosleep(&nap, NULL);

        pthread_mutex_lock(&g_lock);
        g_avail += BLOCK;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

// The guest's data callback: a copy, never a mix. Exactly what the device log's
// 0.002 ms measured.
static int32_t data_cb(void *stream, void *user, void *audio, int32_t frames) {
    (void)stream; (void)user;
    int16_t *out = audio;
    pthread_mutex_lock(&g_lock);
    // What the guest actually does: it serves what it HAS. A block-quantised mixer whose block does not
    // divide the burst hands back a short buffer at every block boundary and
    // leaves the rest of ours untouched — which is zeros, and which is played.
    int32_t serve = g_avail >= frames ? frames
                  : g_avail > 0       ? (int32_t)g_avail : 0;
    if (serve > 0) {
        g_avail -= serve;
        // A sine, INTERLEAVED, with the right channel at half the left's
        // amplitude. The asymmetry is the point: a capture that reports equal
        // peaks on both channels has lost the interleave, and one that reports
        // them the other way round has swapped L and R. Neither is visible in
        // a symmetric test signal, and both are exactly what "the music sounds
        // wrong but every counter is clean" looks like.
        for (int32_t i = 0; i < serve; i++) {
            double t = (double)(g_phase + i) / (double)RATE;
            double v = sin(2.0 * M_PI * g_tone_hz * t);
            out[i * CH + 0] = (int16_t)lrint(v * (double)g_tone_amp);
            if (CH > 1) out[i * CH + 1] = (int16_t)lrint(v * (double)g_tone_amp / 2.0);
        }
        g_phase += serve;
    }
    // else: the buffer arrives zeroed and stays that way — the seam.
    if (g_avail < frames && !g_asked) { g_asked = 1; pthread_cond_signal(&g_cond); }
    pthread_mutex_unlock(&g_lock);
    return 0;   // AAUDIO_CALLBACK_RESULT_CONTINUE
}

// ---------------------------------------------------------------------------

static int fails;
static void ok(int cond, const char *what, double got, double want) {
    printf("%s %-58s got %8.2f  want %8.2f\n", cond ? "  ok  " : "  FAIL", what, got, want);
    if (!cond) fails++;
}

// ---- reading a capture back ----
//
// The point of an instrument is that somebody else can read it. So the test
// reads the WAV the same way the analysis will: off the header, with no
// knowledge of what wrote it. A capture whose header lies is a capture that
// will be analysed wrongly, and this is where that gets caught.
typedef struct {
    unsigned rate, ch;
    size_t   frames;
    long     crossings[2];   // per channel, sign changes
    int      peak[2];
    double   rms[2];
} wav_facts;

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static int wav_read(const char *path, wav_facts *w) {
    memset(w, 0, sizeof *w);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[44];
    if (fread(h, 1, sizeof h, f) != sizeof h) { fclose(f); return 0; }
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4) ||
        rd16(h + 20) != 1 || rd16(h + 34) != 16) { fclose(f); return 0; }
    w->ch   = rd16(h + 22);
    w->rate = rd32(h + 24);
    uint32_t data = rd32(h + 40);
    if (!w->ch || w->ch > 2 || !w->rate) { fclose(f); return 0; }
    w->frames = data / (w->ch * 2u);

    int16_t prev[2] = {0, 0};
    int have_prev = 0;
    for (size_t i = 0; i < w->frames; i++) {
        int16_t fr[2] = {0, 0};
        if (fread(fr, 2, w->ch, f) != w->ch) { w->frames = i; break; }
        for (unsigned c = 0; c < w->ch; c++) {
            int v = fr[c] < 0 ? -fr[c] : fr[c];
            if (v > w->peak[c]) w->peak[c] = v;
            w->rms[c] += (double)fr[c] * (double)fr[c];
            // Count a crossing only between two non-zero samples of opposite
            // sign. A quantised low-amplitude sine sits at exactly 0 for a
            // sample or two at each crossing, and counting "went to zero" as a
            // crossing would double the answer.
            if (have_prev && fr[c] && prev[c] &&
                ((fr[c] > 0) != (prev[c] > 0))) w->crossings[c]++;
            if (fr[c]) prev[c] = fr[c];
        }
        have_prev = 1;
    }
    for (unsigned c = 0; c < w->ch; c++)
        w->rms[c] = w->frames ? sqrt(w->rms[c] / (double)w->frames) : 0.0;
    fclose(f);
    return w->frames > 0;
}

// One capture, checked against the tone that was fed in. Everything asserted
// here is rate-independent on purpose: the device end may be resampled (this
// Mac's output runs at whatever CoreAudio says), so the pitch is checked as
// crossings per SECOND of the file's own declared duration rather than as a
// count.
static void check_capture(const char *label, const char *path, int expect_rate) {
    wav_facts w;
    printf("\n  --- %s: %s\n", label, path);
    if (!wav_read(path, &w)) {
        printf("  FAIL %s: no readable WAV at %s\n", label, path);
        fails++;
        return;
    }
    double dur = (double)w.frames / (double)w.rate;
    printf("        %u Hz, %u ch, %zu frames (%.2f s), peak L/R %d/%d, "
           "rms L/R %.1f/%.1f\n",
           w.rate, w.ch, w.frames, dur, w.peak[0], w.peak[1], w.rms[0], w.rms[1]);

    char what[128];
    snprintf(what, sizeof what, "%s: channels", label);
    ok(w.ch == CH, what, w.ch, CH);
    if (expect_rate) {
        snprintf(what, sizeof what, "%s: sample rate", label);
        ok(w.rate == (unsigned)expect_rate, what, w.rate, expect_rate);
    }
    snprintf(what, sizeof what, "%s: seconds captured", label);
    ok(dur > 1.0, what, dur, 2.0);
    if (dur <= 1.0) return;

    // PITCH. A sine at f crosses zero 2f times a second, in either channel.
    // This is the assertion that fails if the samples are read at the wrong
    // width, byte-swapped, or the stream is spliced out of order.
    for (unsigned c = 0; c < w.ch; c++) {
        double hz = (double)w.crossings[c] / (2.0 * dur);
        snprintf(what, sizeof what, "%s: pitch ch%u (Hz, from zero crossings)",
                 label, c);
        ok(fabs(hz - g_tone_hz) < g_tone_hz * 0.02, what, hz, g_tone_hz);
    }

    // INTERLEAVE and CHANNEL ORDER. The right channel was fed at half the
    // left's amplitude; a swap inverts this ratio and a lost interleave makes
    // it 1.0. Checked on the RMS rather than the peak, because a resampler
    // can overshoot a peak by a hair and cannot move an RMS.
    double ratio = w.rms[0] > 0.0 ? w.rms[1] / w.rms[0] : 0.0;
    snprintf(what, sizeof what, "%s: R/L amplitude ratio", label);
    ok(fabs(ratio - 0.5) < 0.05, what, ratio, 0.5);

    // PURITY. A sine's RMS is its peak over sqrt(2). Anything that mangles
    // sample content — a wrong format read as int16, a stray byte offset —
    // shows up here as a crest factor that is not 1.41.
    double crest = w.rms[0] > 0.0 ? (double)w.peak[0] / w.rms[0] : 0.0;
    snprintf(what, sizeof what, "%s: crest factor ch0 (peak/rms)", label);
    ok(fabs(crest - 1.414) < 0.15, what, crest, 1.414);
}

typedef void *(*fn_new)(void **);
typedef void  (*fn_seti)(void *, int32_t);
typedef void  (*fn_setcb)(void *, void *, void *);
typedef int32_t (*fn_open)(void *, void **);
typedef int32_t (*fn_stream)(void *);

static void *sym(const char *n) {
    void *f = kl_aaudio_sym(n);
    if (!f) { printf("  FAIL missing symbol %s\n", n); exit(1); }
    return f;
}

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 20.0;
    if (getenv("KLAA_MIX_MS")) g_mix_ms = atof(getenv("KLAA_MIX_MS"));
    if (getenv("KLAA_TONE_HZ")) g_tone_hz = atof(getenv("KLAA_TONE_HZ"));

    // Arm both captures BEFORE the stream opens — the sink reads KL_AUDIO_DUMP
    // inside openStream and the feeder reads KL_AAUDIO_DUMP on its first lit
    // pull. Three seconds, so both files have closed themselves (and patched
    // their headers) well before this run ends and the checks read them.
    setenv("KL_AUDIO_DUMP",   "build/t_aaudio-out.wav",   0);
    setenv("KL_AAUDIO_DUMP",  "build/t_aaudio-guest.wav", 0);
    setenv("KL_AUDIO_DUMP_S", "3", 0);

    printf("t_aaudio: a %d-frame mixer block (%.1f ms) behind a %.1f ms mix, "
           "%.0f s; guest tone %.0f Hz at %d LSB (right channel at half)\n",
           BLOCK, 1000.0 * BLOCK / RATE, g_mix_ms, secs, g_tone_hz, g_tone_amp);

    pthread_t mt;
    pthread_create(&mt, NULL, mixer, NULL);

    void *b = NULL;
    ((fn_new)sym("AAudio_createStreamBuilder"))(&b);
    if (!b) { printf("  FAIL builder\n"); return 1; }
    ((fn_seti)sym("AAudioStreamBuilder_setSampleRate"))(b, RATE);
    ((fn_seti)sym("AAudioStreamBuilder_setChannelCount"))(b, CH);
    ((fn_seti)sym("AAudioStreamBuilder_setFormat"))(b, 1);   // PCM_I16
    ((fn_setcb)sym("AAudioStreamBuilder_setDataCallback"))(b, (void *)data_cb, NULL);

    void *s = NULL;
    ((fn_open)sym("AAudioStreamBuilder_openStream"))(b, &s);
    if (!s) { printf("  FAIL openStream\n"); return 1; }
    ((fn_stream)sym("AAudioStream_requestStart"))(s);

    double t0 = now_s();
    while (now_s() - t0 < secs) usleep(200000);
    double elapsed = now_s() - t0;

    ((fn_stream)sym("AAudioStream_requestStop"))(s);

    printf("\n");
    kl_aaudio_report(stdout);
    kl_audio_report(stdout);
    printf("\n");

    // The verdict metric, parsed back out of the census the device also prints.
    // Reading the report rather than a private counter is deliberate: the
    // number this test asserts on has to be the same number a device log is
    // read for, or the two can disagree unnoticed.
    FILE *f = tmpfile();
    kl_aaudio_report(f);
    rewind(f);
    char line[512];
    double silent_pct = -1.0, out_bufs = -1.0;
    double part_bufs = 0.0, part_lit = -1.0;
    int    burst = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "OUTPUT: ");
        if (p) {
            double n = 0, m = 0, pct = 0;
            if (sscanf(p, "OUTPUT: %lf buffer(s) written, %lf of them silent (%lf%%)",
                       &n, &m, &pct) == 3) { out_bufs = n; silent_pct = pct; }
            continue;
        }
        // The burst actually in force, read back rather than assumed — the
        // frames-per-second check below is a rate, and computing it from a
        // constant this file happens to remember is how it would keep passing
        // after the burst changed.
        if ((p = strstr(line, "] burst ")) != NULL) {
            int n = 0;
            if (sscanf(p, "] burst %d fr", &n) == 1 && n > 0) burst = n;
            continue;
        }
        // The partial-fill line. Either form counts: the failure form names how many
        // buffers came back short, the success form says none did.
        if ((p = strstr(line, "PARTIAL FILLS: ")) != NULL) {
            double n = 0, tot = 0, pct = 0, lit = 0;
            if (sscanf(p, "PARTIAL FILLS: %lf of %lf lit buffer(s) (%lf%%) came back "
                          "with only %lf lit", &n, &tot, &pct, &lit) == 4) {
                part_bufs = n; part_lit = lit;
            }
            continue;
        }
        if (strstr(line, "partial fills: none")) { part_bufs = 0.0; part_lit = -1.0; }
    }
    fclose(f);
    if (burst <= 0) burst = KL_AUDIO_NATIVE_FRAMES;

    if (silent_pct < 0) { printf("  FAIL no OUTPUT line in the census\n"); return 1; }

    // 1. The ear's number. Every seam waited out is a buffer of silence the
    //    device never played; dropping and re-pulling alone left this at 16.7%.
    ok(silent_pct < 1.0, "OUTPUT silent %", silent_pct, 0.0);

    // 2. ...and the reason the percentage alone is not enough. A re-pulling
    //    feeder's output is ALSO paced — it wrote a buffer every 5 ms all session — it just wrote
    //    six of them per five the guest composed, so the music ran 20% behind
    //    the wall clock for as long as it played. Real time is the other half
    //    of the fix and it is the half a coalescer cannot reach.
    double wrote_per_s = out_bufs * (double)burst / elapsed;
    ok(fabs(wrote_per_s - RATE) < RATE * 0.04, "frames written per second",
       wrote_per_s, (double)RATE);

    // 2b. The partial-fill assertion, which every check above is blind to. A
    //     buffer with a lit head and a zero tail is lit, is not a hole, costs
    //     no underrun and opens no seam — so it passed 1, 2 and 3 above while
    //     splicing 176 frames of our own memset into the music 40 times a
    //     second. The burst must divide the guest's block, and the way to
    //     know it does is that nothing ever comes back short.
    ok(part_bufs == 0.0, "buffers filled only part way", part_bufs, 0.0);
    if (part_bufs > 0.0)
        printf("        (the short ones carried %.0f of %d frames — %d x %.0f + %.0f "
               "= %.0f is the block the burst has to divide)\n",
               part_lit, burst, (int)(BLOCK / burst), (double)burst,
               (double)(BLOCK % burst), (double)BLOCK);

    // 3. The sink never reached empty doing it. A wait that outruns the lead is
    //    an underrun, which is a worse defect than the one being fixed, so the
    //    bound is asserted and not merely commented.
    ok(kl_audio_underruns() < 20, "CoreAudio underruns", kl_audio_underruns(), 0);

    // 4. ...and the instrument itself, at both ends of the path.
    //    The guest capture must be the tone exactly as composed; the device
    //    capture must still be that tone after arbitration and the resampler.
    //    A difference between the two attributes damage to OUR pipeline, which
    //    is the whole reason there are two files rather than one.
    check_capture("guest capture", getenv("KL_AAUDIO_DUMP"), RATE);
    check_capture("device capture", getenv("KL_AUDIO_DUMP"), 0);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    pthread_mutex_lock(&g_lock);
    g_stop = 1; pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return fails ? 1 : 0;
}
