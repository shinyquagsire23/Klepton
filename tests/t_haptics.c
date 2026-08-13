// t_haptics — the haptics seam's model, end to end and without the guest.
//
// The guest half of this can only be exercised by a real note cut in a real
// session, and the frontend half needs a headset with a Sense controller in
// each hand. What is testable here is everything between: the three OVRPlugin
// entry points as the guest's ABI sees them, the queue behind them, and the
// pulses kl_ovrp_haptics_pull hands out.
//
// The calls go through kl_ovrp_sym, deliberately — that is the pointer the
// guest's dlsym would receive, so the struct returns are exercised through
// the same sret thunk and the same registers the guest uses. A test that
// called the implementations directly would prove nothing about the ABI, which
// is the part of this most likely to be wrong (kl_ovrp.c documents both
// layouts and how each fails when transposed).
//
// The case worth naming: **a vibration stop must not empty the sample queue.**
// Beat Saber calls ovrp_SetControllerVibration(mask, 0, 0) on both hands every
// frame as an idle, while OVRHaptics feeds the buffered path from the same
// managed frame. Serving one by clearing the other's queue would produce
// haptics that are intermittent rather than absent, which is the kind of bug
// that survives a playtest and gets blamed on the hardware.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "kl_ovrp.h"

static int fails;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

// The guest's own structs, as OVRPlugin.cs declares them.
typedef struct { int32_t rate, sample_bytes, safe_queued, min_buf, optimal_buf, max_buf; } hdesc;
typedef struct { int32_t available, queued; } hstate;
typedef struct { const uint8_t *samples; int32_t count; } hbuffer;
// ...and 1.40's, which libhaptics_sdk passes. Declared BY VALUE and called by
// value on purpose: at 32 bytes AAPCS64 makes the caller hand over a pointer to
// a copy, so this is the one way to prove our x1 layout against the compiler's
// rather than against our own reading of it.
typedef struct {
    uint32_t     buffer_size;
    const float *buffer;
    float        rate_hz;
    uint32_t     append;
    uint32_t    *consumed;
} pcmvib;

static hdesc  (*get_desc)(uint32_t);
static hstate (*get_state)(uint32_t);
static int32_t (*set_haptics)(uint32_t, hbuffer);
static int32_t (*set_vibration)(uint32_t, float, float);
static int32_t (*set_pcm)(uint32_t, pcmvib);
static int32_t (*get_rate)(uint32_t, float *);

#define LTOUCH 0x1u
#define RTOUCH 0x2u

static void nap(double seconds) {
    struct timespec ts = { (time_t)seconds, (long)((seconds - (long)seconds) * 1e9) };
    nanosleep(&ts, NULL);
}

static void push(uint32_t mask, uint8_t value, int n) {
    uint8_t buf[512];
    memset(buf, value, sizeof buf);
    hbuffer b = { buf, n };
    set_haptics(mask, b);
}

int main(void) {
    printf("=== haptics seam (M8) ===\n");
    get_desc      = kl_ovrp_sym("ovrp_GetControllerHapticsDesc");
    get_state     = kl_ovrp_sym("ovrp_GetControllerHapticsState");
    set_haptics   = kl_ovrp_sym("ovrp_SetControllerHaptics");
    set_vibration = kl_ovrp_sym("ovrp_SetControllerVibration");
    set_pcm       = kl_ovrp_sym("ovrp_SetControllerHapticsPcm");
    get_rate      = kl_ovrp_sym("ovrp_GetControllerSampleRateHz");
    if (!get_desc || !get_state || !set_haptics || !set_vibration
        || !set_pcm || !get_rate) {
        printf("  FAIL: an entry point did not resolve\n");
        return 1;
    }

    // 1. The descriptor. These are the numbers OVRHaptics sizes its buffer and
    //    paces itself from; a zero anywhere here is what "this controller
    //    cannot vibrate" looks like to the guest.
    hdesc d = get_desc(LTOUCH);
    printf("  desc: %d Hz, %d byte/sample, safe %d, buf %d..%d, optimal %d\n",
           d.rate, d.sample_bytes, d.safe_queued, d.min_buf, d.max_buf, d.optimal_buf);
    CHECK(d.rate == 320, "sample rate %d, expected 320", d.rate);
    CHECK(d.sample_bytes == 1, "sample size %d, expected 1", d.sample_bytes);
    CHECK(d.min_buf >= 1 && d.optimal_buf > d.min_buf && d.max_buf >= d.optimal_buf,
          "buffer sizes not ordered: %d/%d/%d", d.min_buf, d.optimal_buf, d.max_buf);
    // Transposed fields would pass none of the above but WOULD pass "all
    // non-zero", which is why each is checked by value.

    // 2. At rest: nothing queued, the whole buffer free. `available` reading 0
    //    here is the silent failure — the guest clamps what it sends to it.
    hstate s = get_state(LTOUCH);
    printf("  at rest: %d available, %d queued\n", s.available, s.queued);
    CHECK(s.queued == 0, "queued %d at rest", s.queued);
    CHECK(s.available == d.max_buf, "available %d at rest, expected %d",
          s.available, d.max_buf);

    // 3. A buffer the size the guest actually paces to.
    push(LTOUCH, 200, 20);
    s = get_state(LTOUCH);
    CHECK(s.queued == 20, "queued %d after a 20-sample push", s.queued);
    CHECK(s.available == d.max_buf - 20, "available %d after a 20-sample push",
          s.available);
    // ...and the other hand is untouched. One mask bit, one queue.
    hstate r = get_state(RTOUCH);
    CHECK(r.queued == 0, "the right hand queued %d from a left-hand push", r.queued);

    // 4. Nothing has come due yet, so there is nothing to feel yet: the pull
    //    reports what the last window PLAYED, not what is scheduled.
    float amp = 0, secs = 0;
    CHECK(!kl_ovrp_haptics_pull(0, &amp, &secs),
          "a level was reported before any sample had come due");

    // 5. **The bug this model exists to prevent.** Wait out the clip WITHOUT
    //    pulling, exactly as a slow frame would, then pull once. Every one of
    //    those 20 samples has now been retired by the clock — and the hand must
    //    still be told about them. The first implementation dropped whatever
    //    the drain retired, so a note cut evaporated between frames and came
    //    out as a blip or as silence.
    nap(0.08);                             // 20 samples is 62 ms
    s = get_state(LTOUCH);
    CHECK(s.queued == 0, "%d sample(s) left after the clip's own duration", s.queued);
    CHECK(kl_ovrp_haptics_pull(0, &amp, &secs),
          "a clip that played out between two pulls was lost");
    printf("  level: %.2f over %.0f ms\n", (double)amp, (double)secs * 1000);
    CHECK(amp > 0.75f && amp < 0.82f, "amplitude %.3f, expected 200/255", (double)amp);

    // 6. A window is reported once... but ALVR's floor holds the level up for
    //    32 ms after the samples run out, because an actuator cannot act on a
    //    shorter burst. So the drop to silence comes after the hold, not
    //    immediately.
    CHECK(kl_ovrp_haptics_pull(0, &amp, &secs), "the 32 ms hold did not hold");
    nap(0.05);
    CHECK(!kl_ovrp_haptics_pull(0, &amp, &secs), "the level never returned to silence");

    // 7. **The regression guard.** A per-frame vibration stop must leave the
    //    buffered queue alone. This title sends one every frame on both hands
    //    while OVRHaptics is feeding the sample path.
    push(LTOUCH, 255, 20);
    hstate before = get_state(LTOUCH);
    set_vibration(LTOUCH | RTOUCH, 0.0f, 0.0f);
    s = get_state(LTOUCH);
    CHECK(s.queued >= before.queued - 1,
          "a vibration stop emptied the sample queue (%d -> %d)",
          before.queued, s.queued);
    nap(0.08);
    CHECK(kl_ovrp_haptics_pull(0, &amp, &secs),
          "a vibration stop swallowed the buffered clip");
    CHECK(amp > 0.98f, "amplitude %.3f after the stop, expected 1.0", (double)amp);

    // 8. The peak, not the mean. A clip that is mostly silence with one loud
    //    sample is an attack, and reporting its average is how a sharp cut
    //    becomes a soft push.
    {
        uint8_t buf[20];
        memset(buf, 0, sizeof buf);
        buf[3] = 255;
        hbuffer b = { buf, 20 };
        set_haptics(RTOUCH, b);
        nap(0.08);
        CHECK(kl_ovrp_haptics_pull(1, &amp, &secs), "no level from a spiked clip");
        CHECK(amp > 0.98f, "amplitude %.3f from a spiked clip — averaged, not peaked",
              (double)amp);
    }

    // 9. The envelope survives across windows. A ramp pulled twice, half way
    //    through, must report a lower level first and a higher one second —
    //    which is the whole difference between a shaped clip and the flat block
    //    the peak-per-clip version produced.
    nap(0.2);
    (void)kl_ovrp_haptics_pull(0, &amp, &secs);        // clear the hold
    {
        uint8_t ramp[64];
        for (int i = 0; i < 64; i++) ramp[i] = (uint8_t)(i * 4);   // 0 -> 252
        hbuffer b = { ramp, 64 };                                  // 200 ms
        set_haptics(LTOUCH, b);
        nap(0.10);
        float first = 0;
        CHECK(kl_ovrp_haptics_pull(0, &first, &secs), "no level from the ramp's first half");
        nap(0.12);
        float second = 0;
        CHECK(kl_ovrp_haptics_pull(0, &second, &secs), "no level from the ramp's second half");
        printf("  ramp: %.2f then %.2f\n", (double)first, (double)second);
        CHECK(second > first + 0.2f,
              "the ramp came out flat (%.2f then %.2f) — the envelope was collapsed",
              (double)first, (double)second);
    }

    // 10. The queue retires on the wall clock at the rate we told the guest it
    //     would, whether or not anything is draining it. This is what keeps
    //     `available` honest on a host with no actuator at all.
    nap(0.5);
    s = get_state(LTOUCH);
    CHECK(s.queued == 0, "%d sample(s) survived a 500 ms idle", s.queued);
    push(LTOUCH, 100, 64);                 // 200 ms of samples
    s = get_state(LTOUCH);
    CHECK(s.queued == 64, "queued %d immediately after a 64-sample push", s.queued);
    nap(0.30);
    s = get_state(LTOUCH);
    CHECK(s.queued == 0, "%d sample(s) still queued 300 ms after a 200 ms clip",
          s.queued);

    // 11. The level API reads as a level: asserting it once and re-asserting it
    //     every frame behave identically, and it lapses if never refreshed.
    nap(0.1);
    (void)kl_ovrp_haptics_pull(1, &amp, &secs);       // clear any hold
    set_vibration(RTOUCH, 0.5f, 0.75f);
    CHECK(kl_ovrp_haptics_pull(1, &amp, &secs), "a running vibration produced nothing");
    CHECK(amp > 0.73f && amp < 0.77f, "vibration amplitude %.3f, expected 0.75",
          (double)amp);
    set_vibration(RTOUCH, 0.5f, 0.75f);    // the re-assert, as a guest would
    CHECK(kl_ovrp_haptics_pull(1, &amp, &secs), "a re-asserted vibration stopped");
    set_vibration(RTOUCH, 0.0f, 0.0f);
    nap(0.05);                             // past the 32 ms hold
    CHECK(!kl_ovrp_haptics_pull(1, &amp, &secs), "a stopped vibration kept buzzing");

    // 12. The PCM path — 1.40's, and a different producer for the same queue.
    //     libhaptics_sdk (Meta's Haptics SDK, a Rust engine on its own thread)
    //     does not go through OVRHaptics at all: it dlopens libOVRPlugin and
    //     resolves these two by name. Neither was implemented, so the first
    //     haptic the game plays — the pulse a menu button makes when the
    //     pointer highlights it — aborted by name on that thread, which
    //     presented as "the viewer crashes when you hover a button".
    //
    //     The rate is checked against the DESCRIPTOR rather than against 320:
    //     the SDK asks for it and then sends at whatever it was told, so the
    //     two answers disagreeing is a clip played at the wrong speed with
    //     nothing anywhere reporting it.
    nap(0.5);
    (void)kl_ovrp_haptics_pull(0, &amp, &secs);
    (void)kl_ovrp_haptics_pull(1, &amp, &secs);
    {
        // The version comes first, because the SDK reads it while it is
        // initializing its OVRPlugin backend and then formats it into a log
        // line — i.e. it strlen()s whatever we left in its out pointer. The
        // 2-form is `ovrpResult ovrp_GetVersion2(const char **)`, NOT a scalar
        // string return like the un-suffixed one; getting that backwards is a
        // SIGSEGV at 0x0 in _platform_strlen, on the SDK's thread, naming
        // neither haptics nor the version.
        int32_t (*get_version2)(const char **) = kl_ovrp_sym("ovrp_GetVersion2");
        const char *(*get_version)(void)       = kl_ovrp_sym("ovrp_GetVersion");
        const char *ver = (const char *)(uintptr_t)0xdeadbeef;
        CHECK(get_version2 && get_version2(&ver) == 0, "ovrp_GetVersion2 failed");
        CHECK(ver && ver != (const char *)(uintptr_t)0xdeadbeef && *ver,
              "ovrp_GetVersion2 left its out pointer alone — that is a strlen(NULL) "
              "in whatever native code reads it");
        printf("  pcm: OVRPlugin version %s\n", ver ? ver : "(unwritten)");
        CHECK(get_version && strcmp(get_version(), ver) == 0,
              "the two version surfaces disagree");

        float rate = 0;
        CHECK(get_rate(LTOUCH, &rate) == 0, "ovrp_GetControllerSampleRateHz failed");
        printf("  pcm: sample rate %.0f Hz\n", (double)rate);
        CHECK(rate == (float)d.rate, "PCM rate %.0f but the descriptor says %d",
              (double)rate, d.rate);
        // A NULL out pointer is the real plugin's -1001, and it is what its own
        // `cbz x1` answers — so a caller probing with one must not be served.
        CHECK(get_rate(LTOUCH, NULL) != 0, "a NULL sample-rate pointer was served");

        float    clip[32];
        for (int i = 0; i < 32; i++) clip[i] = (float)i / 31.0f;   // 0 -> 1, a ramp
        uint32_t consumed = 0xffffffffu;
        pcmvib   v = { 32, clip, rate, 0 /* replace */, &consumed };
        CHECK(set_pcm(RTOUCH, v) == 0, "ovrp_SetControllerHapticsPcm failed");
        CHECK(consumed == 32, "%u of 32 PCM samples consumed", consumed);
        // It landed in the SAME queue the buffered path feeds, at the same
        // rate, which is the whole reason it is not a fourth source.
        s = get_state(RTOUCH);
        CHECK(s.queued >= 30 && s.queued <= 32, "%d queued after a 32-sample PCM push",
              s.queued);
        CHECK(get_state(LTOUCH).queued == 0, "the PCM push reached the OTHER hand");
        nap(0.09);
        CHECK(kl_ovrp_haptics_pull(1, &amp, &secs), "no level out of a PCM clip");
        printf("  pcm: level %.2f from the ramp's first 90 ms\n", (double)amp);
        CHECK(amp > 0.0f && amp < 0.99f, "a ramp's first third came out at %.2f",
              (double)amp);

        // Append=false is a REPLACE. A clip starting while the last one drains
        // must not be heard behind it.
        pcmvib again = { 32, clip, rate, 0, &consumed };
        set_pcm(RTOUCH, again);
        s = get_state(RTOUCH);
        CHECK(s.queued <= 32, "%d queued after a replacing PCM push — it appended",
              s.queued);

        // ...and the pointers the real plugin refuses (it tests [x1+0x8] and
        // [x1+0x18] before anything else), so a wrong layout here is loud.
        pcmvib bad = { 32, NULL, rate, 0, &consumed };
        CHECK(set_pcm(RTOUCH, bad) != 0, "a NULL PCM buffer was accepted");
        pcmvib bad2 = { 32, clip, rate, 0, NULL };
        CHECK(set_pcm(RTOUCH, bad2) != 0, "a NULL SamplesConsumed was accepted");
    }

    printf(fails ? "\nFAIL: %d check(s)\n" : "\nPASS: the haptics seam behaves\n",
           fails);
    return fails ? 1 : 0;
}
