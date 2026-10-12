// The pose-boundary gate. `make posebound`, in `make check`.
//
// Paint the Town Red VR measured the one-pose invariant BROKEN in gameplay and
// held in the menu — `poses/frame` 1.70 against 1.00 — which is the same split
// as the head-turn judder seen in the headset. The check lives in kl_ovrp.c
// (kl_ovrp_pose_boundary_selftest) because what it asserts is that file's
// frame boundary, driven through the entry points the guest actually calls.
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/xr/kl_ovrp.h"

int main(void) {
    printf("=== one pose per guest frame cycle ===\n");
    int ok = kl_ovrp_pose_boundary_selftest(stdout);
    printf("%s: one guest frame, one head pose, one filed anchor\n",
           ok ? "PASS" : "FAIL");

    printf("=== a frame naming NO eye layer files nothing (Into the Radius loading shake) ===\n");
    int ok2 = kl_ovrp_no_eye_drop_selftest(stdout);
    printf("%s: no-eye frames keep the last real picture and its pose\n",
           ok2 ? "PASS" : "FAIL");
    return ok && ok2 ? 0 : 1;
}
