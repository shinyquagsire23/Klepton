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

static hdesc  (*get_desc)(uint32_t);
static hstate (*get_state)(uint32_t);
static int32_t (*set_haptics)(uint32_t, hbuffer);
static int32_t (*set_vibration)(uint32_t, float, float);

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
    if (!get_desc || !get_state || !set_haptics || !set_vibration) {
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

    // 4. 20 samples is 62 ms, past the 32 ms floor, so it comes out whole.
    float amp = 0, secs = 0;
    CHECK(kl_ovrp_haptics_pull(0, &amp, &secs), "no pulse from a 20-sample push");
    printf("  pulse: %.2f for %.0f ms\n", (double)amp, (double)secs * 1000);
    CHECK(amp > 0.75f && amp < 0.82f, "amplitude %.3f, expected 200/255", (double)amp);
    CHECK(secs > 0.055f && secs < 0.07f, "span %.4f s, expected 20/320", (double)secs);

    // 5. A pull is a DRAIN: the same samples do not come out twice.
    CHECK(!kl_ovrp_haptics_pull(0, &amp, &secs), "the same samples pulled twice");

    // 6. The short-span hold, and that nothing is stranded by it. Five samples
    //    is 15 ms — under the floor — so the first pull holds it back while the
    //    guest may still be feeding, and the next one flushes the tail.
    push(LTOUCH, 128, 5);
    CHECK(!kl_ovrp_haptics_pull(0, &amp, &secs), "a 15 ms span was not held back");
    CHECK(kl_ovrp_haptics_pull(0, &amp, &secs), "the held tail was never flushed");
    CHECK(amp > 0.47f && amp < 0.53f, "tail amplitude %.3f, expected 128/255", (double)amp);

    // 7. **The regression guard.** A per-frame vibration stop must leave the
    //    buffered queue alone. See the header comment.
    //
    //    Measured as a difference, because a pull does not retire anything —
    //    only the clock does, so what is queued here is everything pushed above
    //    that has not yet had its moment. That is the model working, and a test
    //    that asserted an absolute count would be asserting the sleep schedule.
    push(LTOUCH, 255, 20);
    hstate before = get_state(LTOUCH);
    set_vibration(LTOUCH | RTOUCH, 0.0f, 0.0f);
    s = get_state(LTOUCH);
    CHECK(s.queued >= before.queued - 1,
          "a vibration stop emptied the sample queue (%d -> %d)",
          before.queued, s.queued);
    CHECK(kl_ovrp_haptics_pull(0, &amp, &secs),
          "a vibration stop swallowed the buffered pulse");
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
        CHECK(kl_ovrp_haptics_pull(1, &amp, &secs), "no pulse from a spiked clip");
        CHECK(amp > 0.98f, "amplitude %.3f from a spiked clip — averaged, not peaked",
              (double)amp);
    }

    // 9. The queue retires on the wall clock at the rate we told the guest it
    //    would, whether or not anything is draining it. This is what keeps
    //    `available` honest on a host with no actuator at all — and it is also
    //    why everything above could keep pushing without ever running the
    //    buffer dry. Let what is left of that lapse first.
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

    // 10. The level API. It owes a pulse per elapsed chunk rather than one per
    //     call, so a caller re-asserting it every frame does not re-trigger it
    //     every frame — and it lapses on its own if never refreshed.
    set_vibration(RTOUCH, 0.5f, 0.75f);
    CHECK(!kl_ovrp_haptics_pull(1, &amp, &secs),
          "a vibration produced a pulse before any time had passed");
    nap(0.15);
    set_vibration(RTOUCH, 0.5f, 0.75f);    // the re-assert, as a guest would
    CHECK(kl_ovrp_haptics_pull(1, &amp, &secs), "a running vibration produced nothing");
    CHECK(amp > 0.73f && amp < 0.77f, "vibration amplitude %.3f, expected 0.75",
          (double)amp);
    set_vibration(RTOUCH, 0.0f, 0.0f);
    nap(0.15);
    CHECK(!kl_ovrp_haptics_pull(1, &amp, &secs), "a stopped vibration kept pulsing");

    printf(fails ? "\nFAIL: %d check(s)\n" : "\nPASS: the haptics seam behaves\n",
           fails);
    return fails ? 1 : 0;
}
