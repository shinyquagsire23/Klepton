// SL-20 — the OpenXR action-surface gate. `make xrinput`, in `make check`.
//
// The check itself lives in kl_openxr.c (kl_openxr_input_selftest) for the same
// reason `make xrspace`'s does: what it asserts is that file's internal model —
// how a suggested binding decodes to a control, how two hands combine, when an
// action is active — and the pieces it drives are the real entry points the
// guest calls. Testing a copy of them would test the copy.
//
// What this file supplies is the environment and the exit code. The bindings
// the check drives are transcribed from a real run
// (`KL_XR_BINDINGS=1 make slink-vr-run`: 41 suggested for
// oculus/touch_controller, 39 taken), so it exercises the map Steam Link
// actually hands over.
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/kl_openxr.h"

int main(void) {
    // The frontend seam in kl_ovrp parks the hands at a head-relative default
    // until something publishes, and the check publishes absolute positions —
    // so pin the head at the origin, or "the left hand is at x = -0.2" would be
    // a statement about wherever the default head happened to be.
    setenv("KL_OVRP_EYE_HEIGHT", "0", 0);

    printf("=== OpenXR actions, poses and haptics (SL-20) ===\n");
    int ok = kl_openxr_input_selftest(stdout);
    printf("%s: the action surface reads the input kl_ovrp holds\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
