// JKXR: com.drbeef.jkxr.GLES3JNIActivity's own methods.
//
// One family of the synthetic JNIEnv's Java classes. The mechanism (registries,
// dispatch, id interning) is kl_jni.c; this file owns implementations and the
// binding table that names them. See runtime/jni/kl_jni_int.h for the seam.
//
// The whole family is one activity's engine-facing surface, and it divides in
// two: `shutdown`, which ends the process, and seven haptic methods that are
// the port's EXTERNAL ACCESSORY channel — bHaptics vests and ForceTube gun
// stocks, reached over AIDL from the Java.
//
// The controller haptics people actually feel do NOT come through here. The
// engine calls xrApplyHapticFeedback for those, which lands in the OpenXR
// runtime and then in the per-hand CoreHaptics queue like every other target's.
// This is the separate path for hardware bound as an Android service, and the
// Java's own implementation of every one of these methods is a loop over a
// Vector of bound service clients — empty on any headset without that hardware
// installed, which is every headset this project runs on.
//
// So these are no-ops that COUNT, not no-ops that lie: on Android with no
// accessory bound the loop body never executes and the method returns having
// done nothing, which is exactly what happens here. The counts exist because
// the alternative — a silent no-op — would make "the vest never buzzes"
// indistinguishable from "the engine never asked".
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_jni_int.h"

static struct {
    unsigned events, updates, stops, endframes;
    unsigned enables, disables;
    int      enabled;
    char     last_event[64];
} g_hap;

// The event name is the port's own vocabulary — a string like "fire_pistol"
// that an accessory maps to a pattern — and it is the only argument worth
// keeping: position, flags, intensity, angle and height all describe where on a
// body the pattern should play, and there is no body here.
static klj_val jkxr_haptic_event(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    g_hap.events++;
    if (n > 0 && a[0].l)
        snprintf(g_hap.last_event, sizeof g_hap.last_event, "%s", klj_str(a[0].l));
    return (klj_val){.j = 0};
}

static klj_val jkxr_haptic_updateevent(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    g_hap.updates++;
    return (klj_val){.j = 0};
}

static klj_val jkxr_haptic_stopevent(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    g_hap.stops++;
    return (klj_val){.j = 0};
}

// Called once per rendered frame, which makes it the cheapest frame counter on
// this target — the engine owns its own render loop, so nothing else on the
// Java side is called per frame at all.
static klj_val jkxr_haptic_endframe(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    g_hap.endframes++;
    return (klj_val){.j = 0};
}

static klj_val jkxr_haptic_enable(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    g_hap.enables++;
    g_hap.enabled = 1;
    return (klj_val){.j = 0};
}

static klj_val jkxr_haptic_disable(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    g_hap.disables++;
    g_hap.enabled = 0;
    return (klj_val){.j = 0};
}

// The Java is `System.exit(0)` — the engine asking to be closed, which is a
// normal end rather than a failure. It is NOT forwarded to exit() here: the
// driver owns when this process ends (it still has reports to print, and the
// frame pump is on another thread), and a guest thread calling exit from inside
// a JNI call would take the run down mid-report. Recorded and returned; the
// pump sees the quit through the session state machine, as it does on every
// other target.
static klj_val jkxr_shutdown(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    fprintf(stderr, "  [jkxr] the engine called Activity.shutdown() — it is "
                    "asking to quit; the driver decides when this process ends\n");
    return (klj_val){.j = 0};
}

void klj_jkxr_report(FILE *out) {
    if (!out) return;
    if (!g_hap.events && !g_hap.endframes && !g_hap.enables) return;
    fprintf(out, "\n=== JKXR accessory haptics (external services; none bound) ===\n");
    fprintf(out, "  %s; %u event%s (last \"%s\"), %u update%s, %u stop%s, "
                 "%u frame%s, enable/disable %u/%u\n",
            g_hap.enabled ? "enabled by the guest" : "disabled by the guest",
            g_hap.events,    g_hap.events    == 1 ? "" : "s", g_hap.last_event,
            g_hap.updates,   g_hap.updates   == 1 ? "" : "s",
            g_hap.stops,     g_hap.stops     == 1 ? "" : "s",
            g_hap.endframes, g_hap.endframes == 1 ? "" : "s",
            g_hap.enables,   g_hap.disables);
    fflush(out);
}

const klj_binding klj_bind_jkxr[] = {
    {"com/drbeef/jkxr/GLES3JNIActivity", "shutdown", "()V", jkxr_shutdown},
    {"com/drbeef/jkxr/GLES3JNIActivity", "haptic_event",
     "(Ljava/lang/String;IIIFF)V", jkxr_haptic_event},
    {"com/drbeef/jkxr/GLES3JNIActivity", "haptic_updateevent",
     "(Ljava/lang/String;IF)V", jkxr_haptic_updateevent},
    {"com/drbeef/jkxr/GLES3JNIActivity", "haptic_stopevent",
     "(Ljava/lang/String;)V", jkxr_haptic_stopevent},
    {"com/drbeef/jkxr/GLES3JNIActivity", "haptic_endframe", "()V", jkxr_haptic_endframe},
    {"com/drbeef/jkxr/GLES3JNIActivity", "haptic_enable",   "()V", jkxr_haptic_enable},
    {"com/drbeef/jkxr/GLES3JNIActivity", "haptic_disable",  "()V", jkxr_haptic_disable},
    {0}
};
