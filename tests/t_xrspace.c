// The OpenXR reference-space gate. `make xrspace`, in `make check`.
//
// The whole check lives in kl_openxr.c (kl_openxr_space_selftest) because what
// it asserts is that file's internal model of what a space IS, and the pieces
// it composes — klxr_space_pose, klxr_pose_rel — are the ones the two locate
// entry points call. Testing a copy of them would test the copy.
//
// What this file supplies is the environment the check needs and the exit code:
// an eye separation, so "the eyes are a few centimetres from the head" is a
// statement about a real offset rather than about zero.
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/xr/kl_openxr.h"

int main(void) {
    // Without a frontend there is no measured IPD, and klovrp_eye_offset then
    // answers 0 for both eyes — which would let a runtime that leaks the head's
    // position pass, because both leaked answers would still agree with each
    // other. setenv rather than an assignment: the offset is latched on first
    // use, and 0 (do not overwrite) so a run can still A/B a different value.
    setenv("KL_OVRP_IPD", "0.063", 0);

    printf("=== OpenXR reference spaces ===\n");
    int ok = kl_openxr_space_selftest(stdout);
    printf("%s: the space algebra holds\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
