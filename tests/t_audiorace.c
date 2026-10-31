// Writers against a device that keeps being reopened, closed, flushed and
// restarted under them. `make audio-race`, in `make check`.
//
// The ring, the owner's resampler and the slot table are freed and replaced by
// sink_open, kl_audio_close and kl_audio_restart, and a writer may be inside its
// wait for room at that moment — holding g_wlock, or (KL_AUDIO_MIX_UNLOCK)
// having let go of it for a nap. The lock order in kl_audio.c (g_prod -> g_lock
// -> g_wlock, nothing takes g_lock under g_wlock) is what keeps that safe and
// free of deadlock; this drives it:
//
//   - two writer threads, a float stream (960 frames a write) and an int16 one
//     (256), as Steam Link has;
//   - short-lived writer threads that write a few buffers and exit (the
//     per-thread mix buffers must be freed with them);
//   - one thread that reopens the device at changing rates, channel counts and
//     sample formats, closes it, flushes it and restarts it, as fast as it can.
//
// Two processes: sources MIXED with the unlocked wait (Steam Link's path), and
// one owner (every other title's). Each must finish without a crash and within
// a deadline — a deadlock is a timeout.
//
// A case runs until it has done a fixed amount of work (MIN_REOPENS rounds,
// MIN_WRITES writes, MIN_SHORT short-lived threads), not for a fixed time:
// opening the output device is slow on a loaded Mac, and a 3-second run there
// once did 5 rounds and failed a count it would have passed given longer. The
// clock only bounds the run, for the deadlock. `make audio-race-asan` builds it with
// AddressSanitizer. Opens the real output device; a host without one SKIPs.
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "media/kl_audio.h"

#define MIN_SECS    3    // at least this long, so writers and churn overlap
#define MIN_REOPENS 20
#define MIN_WRITES  200
#define MIN_SHORT   8
#define DEADLINE_S  60

static atomic_int g_stop;
static atomic_ulong g_writes, g_reopens, g_short_threads;
static char g_tag_f32, g_tag_s16;

static void nap_us(unsigned us) {
    struct timespec ts = { 0, (long)us * 1000L };
    nanosleep(&ts, NULL);
}

typedef struct { const void *tag; int f32; int burst; int count; } writer;

static void write_some(const writer *w) {
    static const double step = 2.0 * M_PI * 440.0 / 48000.0;
    float   fbuf[2 * 960];
    int16_t sbuf[2 * 960];
    double ph = 0.0;
    for (int n = 0; w->count < 0 || n < w->count; n++) {
        if (atomic_load(&g_stop)) return;
        for (int i = 0; i < w->burst; i++, ph += step) {
            float v = 0.001f * (float)sin(ph);
            fbuf[2 * i] = fbuf[2 * i + 1] = v;
            sbuf[2 * i] = sbuf[2 * i + 1] = (int16_t)(v * 32767.0f);
        }
        size_t bytes = (size_t)w->burst * 2 * (w->f32 ? sizeof(float) : sizeof(int16_t));
        size_t got = kl_audio_write_src(w->tag, w->f32 ? (void *)fbuf : (void *)sbuf, bytes);
        atomic_fetch_add(&g_writes, 1);
        if (!got) nap_us(500);              // no device right now
    }
}

static void *writer_thread(void *arg) { write_some(arg); return NULL; }

// Threads that come and go, each a stream of its own for a few buffers.
static void *short_lived(void *unused) {
    (void)unused;
    while (!atomic_load(&g_stop)) {
        static char tags[8];
        for (int i = 0; i < 8 && !atomic_load(&g_stop); i++) {
            kl_audio_open_src_fmt(&tags[i], "aaudio", 48000, 2, KL_AUDIO_F32);
            kl_audio_play_src(&tags[i]);
            writer w = { &tags[i], 1, 256, 4 };
            pthread_t t;
            if (pthread_create(&t, NULL, writer_thread, &w) == 0) {
                pthread_join(t, NULL);
                atomic_fetch_add(&g_short_threads, 1);
            }
            kl_audio_pause_src(&tags[i]);
            kl_audio_close_src(&tags[i]);
        }
    }
    return NULL;
}

static void *churn(void *unused) {
    (void)unused;
    static const struct { unsigned rate, ch; kl_audio_fmt fmt; } F[] = {
        { 48000, 2, KL_AUDIO_F32 }, { 44100, 2, KL_AUDIO_S16 },
        { 48000, 1, KL_AUDIO_S16 }, { 22050, 2, KL_AUDIO_F32 },
        { 96000, 2, KL_AUDIO_S32 },
    };
    unsigned i = 0;
    while (!atomic_load(&g_stop)) {
        kl_audio_open_fmt(F[i % 5].rate, F[i % 5].ch, F[i % 5].fmt);
        kl_audio_play();
        nap_us(1000 + (i % 7) * 700);
        switch (i % 4) {
        case 0: kl_audio_flush(); break;
        case 1: kl_audio_restart(); break;
        case 2: kl_audio_close(); break;
        case 3: kl_audio_flush_src(&g_tag_f32); kl_audio_pause_src(&g_tag_s16);
                kl_audio_play_src(&g_tag_s16); break;
        }
        nap_us(500 + (i % 5) * 400);
        atomic_fetch_add(&g_reopens, 1);
        i++;
    }
    return NULL;
}

static void on_alarm(int sig) {
    (void)sig;
    static const char msg[] = "FAIL: did not finish within the deadline — a deadlock, "
                              "or the writers or the churn made no progress\n";
    write(STDOUT_FILENO, msg, sizeof msg - 1);
    _exit(2);
}

static int run(int mixed) {
    signal(SIGALRM, on_alarm);
    alarm(DEADLINE_S);
    if (mixed) {
        setenv("KL_AUDIO_MIX_SOURCES", "1", 1);
        setenv("KL_AUDIO_MIX_UNLOCK", "1", 1);
    } else {
        setenv("KL_AUDIO_MIX_SOURCES", "0", 1);
    }
    setenv("KL_AUDIO_REPORT_SECS", "0", 1);
    printf("--- %s ---\n", mixed ? "sources mixed, wait outside the lock (Steam Link)"
                                : "one owner (every other title)");
    fflush(stdout);

    if (kl_audio_open_src_fmt(&g_tag_f32, "aaudio", 48000, 2, KL_AUDIO_F32) != 0 ||
        !kl_audio_active()) {
        printf("SKIP: no output device on this host\n");
        return 1;
    }
    kl_audio_open_src_fmt(&g_tag_s16, "opensl", 48000, 2, KL_AUDIO_S16);
    kl_audio_play_src(&g_tag_f32);
    kl_audio_play_src(&g_tag_s16);

    writer a = { &g_tag_f32, 1, 960, -1 }, b = { &g_tag_s16, 0, 256, -1 };
    pthread_t ta, tb, tc, ts;
    pthread_create(&ta, NULL, writer_thread, &a);
    pthread_create(&tb, NULL, writer_thread, &b);
    pthread_create(&tc, NULL, churn, NULL);
    pthread_create(&ts, NULL, short_lived, NULL);
    // Until both the time and the work are done; a run that never gets there is
    // the deadline's to report.
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        nap_us(10000);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec >= MIN_SECS &&
            atomic_load(&g_reopens) >= MIN_REOPENS && atomic_load(&g_writes) >= MIN_WRITES &&
            atomic_load(&g_short_threads) >= MIN_SHORT)
            break;
    }
    atomic_store(&g_stop, 1);
    kl_audio_pause();                       // lets a writer out of its wait at once
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    pthread_join(tc, NULL);
    pthread_join(ts, NULL);
    kl_audio_close_src(&g_tag_f32);
    kl_audio_close_src(&g_tag_s16);
    kl_audio_close();
    alarm(0);
    printf("  %lu writes, %lu reopen/close/flush/restart rounds, %lu short-lived writer "
           "threads — no crash, no deadlock\n",
           atomic_load(&g_writes), atomic_load(&g_reopens), atomic_load(&g_short_threads));
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== audio sink: writers against reopen/close/restart ===\n");
    int all = 0, skipped = 0;
    for (int mixed = 1; mixed >= 0; mixed--) {
        fflush(stdout);
        fflush(stderr);
        pid_t pid = fork();
        if (pid == 0) _exit(run(mixed));
        int st = 0;
        if (pid < 0 || waitpid(pid, &st, 0) < 0) { printf("FAIL: fork\n"); return 1; }
        if (WIFSIGNALED(st)) {
            printf("FAIL: the %s case died with signal %d\n", mixed ? "mixed" : "owner",
                   WTERMSIG(st));
            all = 1;
        } else if (WEXITSTATUS(st) == 1) {
            skipped = 1;
        } else if (WEXITSTATUS(st) != 0) {
            printf("FAIL: the %s case (exit %d)\n", mixed ? "mixed" : "owner", WEXITSTATUS(st));
            all = 1;
        }
    }
    if (all) return 1;
    printf(skipped ? "audio race: SKIPPED (no output device)\n" : "audio race OK\n");
    return 0;
}
