// The floor gate. `make ovrpfloor`, in `make check`.
//
// Pinball FX VR's crash and its camera in the floor were one defect seen
// twice: visionOS's ARKit origin sits at the device's own start pose — at EYE
// level — while every guest that asks for a FloorLevel tracking origin reads
// the poses we serve as though y=0 were the ground. Pinball FX VR switched
// origins when its lobby loaded, asked ovrp_GetUserEyeHeight2 for the number it
// needed to place the camera, and died on an unimplemented name.
//
// **The first fix was measured and it was one half too many.** Answering
// ovrp_GetUserEyeHeight2 AND raising the poses by the same number puts the
// camera 3.2 m up, because the reason a guest asks how tall the user is IS to
// build its own floor offset out of the answer. On the headset the camera went
// from standing in the lobby floor to floating above it, in one change. So
// the shipping shape is: the getter answers, the pose is handed back
// untouched, and KL_OVRP_FLOOR_OFFSET is selected by nothing.
//
// That makes this gate's job two-sided, and it now runs BOTH sides:
//
//   knob OFF (the shipping shape, every target) — the getter still answers a
//       standing height, and the FloorLevel pose comes back exactly as given.
//       This is the direction that would have caught the overshoot.
//   knob ON  (the A/B, for a guest that never asks) — the raise is exactly the
//       number the getter reports, so the two can never drift apart.
//
// The whole check lives in kl_ovrp.c (kl_ovrp_floor_selftest) for the same
// reason t_xrspace's does: what it asserts is that file's model of what the
// tracking space IS, and it composes the shipping entry points —
// ovrp_GetUserEyeHeight2 and klovrp_GetNodePoseState_impl — rather than a copy
// of their arithmetic.
//
// The two modes are two PROCESSES rather than two calls, because the knob is
// read once into a static — as it is in the shipping build, on purpose. A fork
// taken before either side has touched it gives each a clean first read.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../runtime/xr/kl_ovrp.h"

static int run(const char *knob, const char *title) {
    setenv("KL_OVRP_FLOOR_OFFSET", knob, 1);
    // The getter's `standing` answer (Pinball FX VR's), which is what this gate
    // asserts; the default is `origin` — see klovrp_user_eye_height_standing.
    setenv("KL_OVRP_USER_EYE_HEIGHT", "standing", 1);
    printf("=== the floor under an OVRPlugin guest — %s ===\n", title);
    int ok = kl_ovrp_floor_selftest(stdout);
    printf("%s: %s\n", ok ? "PASS" : "FAIL",
           knob[0] == '0'
               ? "the getter answers a standing height and the FloorLevel pose "
                 "is handed back untouched"
               : "the standing height the getter reports is the standing height "
                 "the poses are raised by");
    fflush(stdout);
    return ok;
}

int main(void) {
    // The shipping shape first, in a child, so its static read of the knob
    // cannot be the parent's.
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
        _exit(run("0", "KL_OVRP_FLOOR_OFFSET off, the shipping shape") ? 0 : 1);
    if (pid < 0) {
        printf("FAIL: fork() failed — this gate needs two processes\n");
        return 1;
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("FAIL: the KL_OVRP_FLOOR_OFFSET=0 half of the gate did not pass "
               "— the getter must answer while the pose stays untouched\n");
        return 1;
    }
    printf("\n");
    int ok = run("1", "KL_OVRP_FLOOR_OFFSET on, the A/B");
    printf("%s: the floor gate — both directions\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
