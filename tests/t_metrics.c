// kl_metrics.c: the numbers behind the panel in the headset (KL_STATS_CARD).
//
// A stand-in compositor presents 100 pictures a second for a little over two
// seconds, every fourth of them a repeat of the game's last one, while one
// named thread keeps a core busy. The samples must say so: the rates within a
// tenth, the busy thread found and named, the memory and the eye size filled
// in — and nothing at all before the counting was begun.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "kl_metrics.h"

static volatile int g_stop;

static void nap_ns(long ns) {
    struct timespec t = { 0, ns };
    nanosleep(&t, NULL);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void *compositor(void *arg) {
    (void)arg;
    // Against the clock and not by sleeping a period at a time: a sleep runs long, and the rate with it.
    double t0 = now_s();
    for (unsigned n = 0; !g_stop; n++) {
        while (now_s() - t0 < (double)n / 100.0) nap_ns(500000);
        int again = n % 4 == 3;
        kl_metrics_note_shown(!again, again);
        kl_metrics_note_composite(0.001);
    }
    return NULL;
}

static void *busy(void *arg) {
    (void)arg;
    pthread_setname_np("Busy Bee");
    volatile double x = 1;
    while (!g_stop) x = x * 1.0000001 + 1e-9;
    return NULL;
}

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); return 1; } } while (0)
static int near(float v, float want) { return fabsf(v - want) <= want * 0.1f; }

// The two menu buttons, polled 100 times a second on a clock of the test's own. `l` and `r` say from when
// to when each is down; what comes back is how long each press reached the game and how often the panel
// was switched.
typedef struct { double from, to; } span;
static double g_clock = 100.0;
static int g_taps;                      // the polls that ended a tap of the two together (kl_metrics_chord's 2)
static void hold(span l, span r, double seconds, double *passed_l, double *passed_r, int *switched) {
    *passed_l = *passed_r = 0; *switched = 0; g_taps = 0;
    for (int i = 0; i < (int)(seconds * 100 + 0.5); i++) {
        double t = i / 100.0;
        int pl = 0, pr = 0;
        int got = kl_metrics_chord(t >= l.from && t < l.to, t >= r.from && t < r.to, g_clock, &pl, &pr);
        if (got == 1) (*switched)++;
        else if (got == 2) g_taps++;
        if (pl) *passed_l += 0.01;
        if (pr) *passed_r += 0.01;
        g_clock += 0.01;
    }
}

static int chord(void) {
    const span none = { 9, 9 };
    double pl, pr; int sw;
    // Not shown until someone says so; here the game's setting does.
    CHECK(kl_metrics_visible() == 0, "the panel is shown before anyone asked for it");
    kl_metrics_set_visible(1);
    CHECK(kl_metrics_visible() == 1, "the panel was switched on and is not shown");

    // One button alone is the game's: a long press arrives a quarter of a second late and ends with it...
    hold((span){ 0.1, 1.1 }, none, 2.0, &pl, &pr, &sw);
    CHECK(fabs(pl - 0.75) < 0.03 && pr == 0 && sw == 0, "a 1 s press of the left button reached the game for %.2f s "
          "(right %.2f, %d switch)", pl, pr, sw);
    // ...and a tap too short to wait out is played afterwards.
    hold(none, (span){ 0.1, 0.18 }, 1.0, &pl, &pr, &sw);
    CHECK(pl == 0 && fabs(pr - 0.10) < 0.03 && sw == 0, "an 80 ms tap of the right button reached the game for %.2f s",
          pr);

    // Both, the second 80 ms after the first, held for a second and a half: the panel goes, once, and the
    // game hears of neither — not while they are held and not as they are let go one after the other.
    hold((span){ 0.10, 1.60 }, (span){ 0.18, 1.90 }, 3.0, &pl, &pr, &sw);
    CHECK(sw == 1 && kl_metrics_visible() == 0, "both held 1.4 s: %d switch(es), visible %d", sw, kl_metrics_visible());
    CHECK(g_taps == 0, "a hold that switched the panel was also taken for a tap");
    // A tap of the two while the panel is hidden is nothing at all: no switch, no tap, nothing for the game.
    hold((span){ 0.10, 0.30 }, (span){ 0.12, 0.30 }, 1.0, &pl, &pr, &sw);
    CHECK(sw == 0 && g_taps == 0 && pl == 0 && pr == 0, "a tap of both with the panel hidden: %d switch(es), "
          "%d tap(s), passed %.2f/%.2f", sw, g_taps, pl, pr);
    CHECK(pl == 0 && pr == 0, "both held: the game was passed the left for %.2f s and the right for %.2f s", pl, pr);
    // The same again brings it back.
    hold((span){ 0.10, 1.40 }, (span){ 0.10, 1.40 }, 2.0, &pl, &pr, &sw);
    CHECK(sw == 1 && kl_metrics_visible() == 1 && pl == 0 && pr == 0, "both held again: %d switch(es), visible %d", sw,
          kl_metrics_visible());

    // Both for half a second does not switch the panel and nothing reaches the game; with the panel shown it
    // is a tap of the two together, said once (a test build switches XR head prediction on it).
    hold((span){ 0.10, 0.60 }, (span){ 0.10, 0.60 }, 1.5, &pl, &pr, &sw);
    CHECK(sw == 0 && pl == 0 && pr == 0 && kl_metrics_visible() == 1, "both for 0.5 s: %d switch(es), passed %.2f/%.2f",
          sw, pl, pr);
    CHECK(g_taps == 1, "both for 0.5 s with the panel shown: %d tap(s), not one", g_taps);
    // Together for 0.6 s, one let go and pressed again: that is not a second held together.
    hold((span){ 0.10, 2.00 }, (span){ 0.10, 0.70 }, 1.0, &pl, &pr, &sw);
    hold((span){ 0.00, 1.00 }, (span){ 0.00, 0.80 }, 2.0, &pl, &pr, &sw);
    CHECK(sw == 0 && kl_metrics_visible() == 1, "a hold with a gap in it switched the panel");
    CHECK(g_taps == 0, "a hold of nearly two seconds with a gap in it was taken for a tap");
    printf("  the two menu buttons: alone they are the game's, together for a second they switch the panel\n");
    return 0;
}

int main(void) {
    printf("=== metrics: a second of how the game ran ===\n");
    kl_metrics m;
    memset(&m, 0x55, sizeof m);
    kl_metrics_note_shown(1, 0);
    CHECK(kl_metrics_tick() == 0 && kl_metrics_latest(&m) == 0, "a sample was taken before the counting was begun");

    kl_metrics_begin();
    kl_metrics_note_eye(3072, 2880);
    kl_metrics_note_upscale(2, 4096, 3840, "Vulkan \xc2\xb7 per eye", NULL);
    pthread_t tc, tb;
    pthread_create(&tc, NULL, compositor, NULL);
    pthread_create(&tb, NULL, busy, NULL);
    int taken = 0;
    for (int i = 0; i < 14; i++) {              // 3.5 s at the heartbeat's four ticks a second
        taken += kl_metrics_tick();
        nap_ns(250000000);
    }
    g_stop = 1;
    pthread_join(tc, NULL);
    pthread_join(tb, NULL);

    unsigned seq = kl_metrics_latest(&m);
    printf("  %d sample(s), the last: %.1f fps of %.1f shown, %.1f repeated; CPU %.2f cores, busiest \"%s\" %.0f%%; "
           "composite %.2f ms; GPU busy %.3f; memory %d MB used, %d left; eye %dx%d\n",
           taken, m.fps, m.display_hz, m.stale, m.cpu_cores, m.cpu_top_name, m.cpu_top * 100.0f, m.gpu_comp_ms,
           m.gpu_busy, m.mem_used_mb, m.mem_left_mb, m.eye_w, m.eye_h);
    CHECK(taken >= 2 && taken <= 3, "3.5 s should hold two or three samples, not %d", taken);
    CHECK(seq == (unsigned)taken, "seq %u after %d samples", seq, taken);
    CHECK(near(m.display_hz, 100), "100 pictures a second were presented, the sample says %.1f", m.display_hz);
    CHECK(near(m.fps, 75), "75 of them were new, the sample says %.1f", m.fps);
    CHECK(near(m.stale, 25), "25 of them were repeats, the sample says %.1f", m.stale);
    CHECK(m.game_fps == 0, "no game frame was counted, the sample says %.1f", m.game_fps);
    CHECK(near(m.gpu_comp_ms, 1.0f), "each composite pass took 1 ms, the sample says %.2f", m.gpu_comp_ms);
    CHECK(m.cpu_top >= 0.5f && m.cpu_top <= 1.0f, "a thread that never rests reads as %.0f%% of a core", m.cpu_top * 100.0f);
    CHECK(strcmp(m.cpu_top_name, "Busy Bee") == 0, "the busiest thread is \"Busy Bee\", the sample names \"%s\"",
          m.cpu_top_name);
    CHECK(m.cpu_cores >= m.cpu_top, "the process (%.2f cores) used less than its busiest thread (%.2f)", m.cpu_cores,
          m.cpu_top);
    CHECK(m.mem_used_mb > 0, "the footprint is missing");
    CHECK(m.eye_w == 3072 && m.eye_h == 2880, "the eye size is %dx%d", m.eye_w, m.eye_h);
    CHECK(m.upscale == 2 && m.out_w == 4096 && m.out_h == 3840, "the upscale is %d to %dx%d", m.upscale, m.out_w,
          m.out_h);
    CHECK(strcmp(m.path, "Vulkan \xc2\xb7 per eye") == 0 && m.upscale_note[0] == 0, "the path is \"%s\", the note \"%s\"",
          m.path, m.upscale_note);
    // No command buffer was committed here: the GPU, where there is one to count on, did nothing for us.
    CHECK(m.gpu_busy <= 0.001f, "no command buffer was committed and the GPU reads %.3f busy", m.gpu_busy);
    CHECK(m.compiles == 0 && m.compiles_cached == 0, "no shader was asked for and the sample says %.1f compiled, "
          "%.1f cached a second", m.compiles, m.compiles_cached);
    // A tap of Create + Options switches MetalFX; the panel draws the compositor's next word on it at once, from
    // kl_metrics_upscale_now, before the next sample has it.
    kl_metrics_note_upscale(0, 0, 0, "Vulkan \xc2\xb7 per eye", NULL);
    kl_metrics now = m;
    kl_metrics_upscale_now(&now);
    CHECK(now.upscale == 0 && now.out_w == 0 && now.out_h == 0 && strcmp(now.path, "Vulkan \xc2\xb7 per eye") == 0,
          "after the switch kl_metrics_upscale_now reads %d to %dx%d on \"%s\", not 0 to 0x0 on the same path",
          now.upscale, now.out_w, now.out_h, now.path);
    CHECK(now.fps == m.fps && now.seq == m.seq, "kl_metrics_upscale_now touched more than the upscale");
    kl_metrics latest;
    kl_metrics_latest(&latest);
    // ...while the last sample keeps what was true when it was taken, until the next second.
    CHECK(latest.upscale == 2, "the last sample was rewritten by the switch: it reads upscale %d, not the 2 it was "
          "taken with", latest.upscale);
    if (chord()) return 1;
    printf("metrics: PASS\n");
    return 0;
}
