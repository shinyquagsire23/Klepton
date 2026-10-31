// Two guest streams MIXED — Steam Link's shape (community fix #49).
//
// During a stream Steam Link keeps two AAudio output streams open: the 2D
// shell's SDL3 one (float, 960 frames a write) and the VR client's own (int16,
// 256), and both carry sound. With KL_AUDIO_MIX_SOURCES on (the default for
// steamlink-vr) the sink must let both through, each converted from its own
// format, and each must deliver real time: a writer that is starved shows as a
// share of real time below 100% and as resyncs, which is what the sound
// cutting out every few seconds looked like in the log.
//
// The gates:
//   1. both writers finish their seconds of audio in about that much wall time
//      (the device paces them, neither waits for the other);
//   2. the sink's per-source lines say both delivered real time, without
//      falling behind the reader;
//   3. one stream pausing does not stop the device under the other.
//
// Opens the real output device (quietly: -40 dBFS), so it is NOT in `check`.
//   make audio-mix
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "media/kl_audio.h"

static int g_fail;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define RATE 48000
#define SECS 3

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static char g_tag_f32, g_tag_s16;

typedef struct {
    const void *tag;
    int         f32;        // float stream, else int16
    int         burst;      // frames a write
    double      hz;
    double      secs;
    double      took;       // wall time
    size_t      short_writes;
} writer;

static void *run(void *arg) {
    writer *w = arg;
    size_t total = (size_t)(w->secs * RATE), done = 0;
    float   fbuf[2 * 960];
    int16_t sbuf[2 * 960];
    double ph = 0.0, step = 2.0 * M_PI * w->hz / RATE;
    double t0 = now_s();
    while (done < total) {
        int n = w->burst;
        for (int i = 0; i < n; i++, ph += step) {
            float v = 0.01f * (float)sin(ph);
            if (w->f32) { fbuf[2 * i] = v; fbuf[2 * i + 1] = v; }
            else { sbuf[2 * i] = (int16_t)(v * 32767.0f); sbuf[2 * i + 1] = sbuf[2 * i]; }
        }
        size_t bytes = (size_t)n * 2 * (w->f32 ? sizeof(float) : sizeof(int16_t));
        size_t got = kl_audio_write_src(w->tag, w->f32 ? (void *)fbuf : (void *)sbuf, bytes);
        if (got < bytes) w->short_writes++;
        done += (size_t)n;
    }
    w->took = now_s() - t0;
    return NULL;
}

// The sink's live report, captured, for its per-source lines.
static double g_share[4];
static unsigned long g_resyncs[4];
static int read_report(void) {
    FILE *tmp = tmpfile();
    if (!tmp) return 0;
    kl_audio_report(tmp);
    rewind(tmp);
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, tmp)) {
        fputs(line, stdout);
        int i; double share; unsigned long rs;
        const char *p = strstr(line, "src");
        if (p && sscanf(p, "src%d: %lf%% of real time since it came, %lu resync",
                        &i, &share, &rs) == 3 && i >= 0 && i < 4) {
            g_share[i] = share; g_resyncs[i] = rs;
            if (i + 1 > n) n = i + 1;
        }
    }
    fclose(tmp);
    return n;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setenv("KL_AUDIO_MIX_SOURCES", "1", 1);
    setenv("KL_AUDIO_MIX_UNLOCK", "1", 1);
    setenv("KL_AUDIO_REPORT_SECS", "0", 1);
    printf("=== two streams mixed (KL_AUDIO_MIX_SOURCES) ===\n");

    if (kl_audio_open_src_fmt(&g_tag_f32, "aaudio", RATE, 2, KL_AUDIO_F32) != 0 ||
        kl_audio_open_src_fmt(&g_tag_s16, "aaudio", RATE, 2, KL_AUDIO_S16) != 0 ||
        !kl_audio_active()) {
        printf("SKIP: no output device on this host\n");
        return 0;
    }
    kl_audio_play_src(&g_tag_f32);
    kl_audio_play_src(&g_tag_s16);

    writer a = { &g_tag_f32, 1, 960, 440.0, SECS, 0, 0 };
    writer b = { &g_tag_s16, 0, 256, 660.0, SECS, 0, 0 };
    pthread_t ta, tb;
    pthread_create(&ta, NULL, run, &a);
    pthread_create(&tb, NULL, run, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    // The ring holds the latency target ahead, so a writer finishes that much
    // before its audio has played; anything near SECS is real time.
    CHECK(a.took > SECS - 0.5 && a.took < SECS + 0.5,
          "float stream (960/write): %d s of audio in %.2f s, %zu short write(s)",
          SECS, a.took, a.short_writes);
    CHECK(b.took > SECS - 0.5 && b.took < SECS + 0.5,
          "int16 stream (256/write): %d s of audio in %.2f s, %zu short write(s)",
          SECS, b.took, b.short_writes);
    CHECK(a.short_writes == 0 && b.short_writes == 0, "no short writes");

    int n = read_report();
    CHECK(n == 2, "the report has a line for each of the two sources (%d)", n);
    for (int i = 0; i < n && i < 2; i++) {
        CHECK(g_share[i] > 90.0, "src%d delivered %.0f%% of real time", i, g_share[i]);
        CHECK(g_resyncs[i] == 0, "src%d fell behind the reader %lu time(s)", i, g_resyncs[i]);
    }

    // 3. The float stream stops; the int16 one plays on in real time.
    kl_audio_pause_src(&g_tag_f32);
    writer c = { &g_tag_s16, 0, 256, 660.0, 1.0, 0, 0 };
    run(&c);
    CHECK(c.took > 0.6 && c.took < 1.5 && c.short_writes == 0,
          "after the other stream paused: 1 s of audio in %.2f s, %zu short write(s)",
          c.took, c.short_writes);

    // ...and closing it leaves the device open for the survivor.
    kl_audio_close_src(&g_tag_f32);
    CHECK(kl_audio_active(), "the device stays open while a source remains");
    kl_audio_pause_src(&g_tag_s16);
    kl_audio_close_src(&g_tag_s16);
    CHECK(!kl_audio_active(), "the device closes with the last source");

    if (g_fail) { printf("FAILED: %d check(s)\n", g_fail); return 1; }
    printf("audio mix OK\n");
    return 0;
}
