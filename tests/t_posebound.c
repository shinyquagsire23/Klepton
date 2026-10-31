// The pose-boundary gate for one pose per guest frame. `make posebound`, in
// `make check`.
//
// From rebelancap's fork (tests/t_posebound.c, his commit a0e966f). Paint the
// Town Red VR measured the one-pose invariant BROKEN in gameplay and held in
// the menu — `poses/frame` 1.70 against 1.00 — which is the same split as the
// head-turn judder seen in the headset. The check lives in kl_ovrp.c
// (kl_ovrp_pose_boundary_selftest) because what it asserts is that file's frame
// boundary, driven through the entry points the guest actually calls.
//
// One pose per guest frame is always on, for a guest that delimits its frames
// through OVRPlugin. The gate still has more than one side, and each side is a
// PROCESS, because the boundary is read once into a static and the feature's
// state lives for the process — as in the shipping build. Every case is a
// child forked before anything has touched kl_ovrp, with the environment set
// for it:
//
//   (a) the shipping default. One head pose per guest frame cycle; the frame
//       record carries it.
//   (b) KL_OVRP_POSE_BOUNDARY=index — his finding. The same call pattern splits
//       every cycle across two poses. This case passes when it SEES the split:
//       it is the proof that (a) can fail.
//   (c) a guest that never speaks OVRPlugin (OpenXR): one-pose is in force
//       only for a guest that calls ovrp_WaitToBeginFrame, so here it must do
//       nothing — the latch's answers, nothing frozen.
//   (d) Walkabout's shape: an OpenXR frame loop whose Meta SDK calls
//       ovrp_Update2(step 0, frameIdx 0) in mid-frame. Before one-pose was
//       gated on the frame cycle each such call re-froze the head the OpenXR
//       queries read. The head before and after it must be one pose.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../runtime/xr/kl_ovrp.h"

static const struct {
    const char *tag, *boundary;
    int which;
    const char *title, *verdict;
} CASES[] = {
    { "(a)", NULL, KL_OVRP_POSEBOUND_CYCLE,
      "the shipping default, boundary = the guest's frame cycle",
      "one guest frame cycle, one head pose, one filed anchor" },
    { "(b)", "index", KL_OVRP_POSEBOUND_INDEX,
      "KL_OVRP_POSE_BOUNDARY=index — must SPLIT",
      "keyed on the frame index the same pattern splits into two poses a "
      "cycle (the gate can fail)" },
    { "(c)", NULL, KL_OVRP_POSEBOUND_XR,
      "a guest that never speaks OVRPlugin (OpenXR)",
      "one-pose does nothing there: latch pose, live eye transform, nothing "
      "frozen" },
    { "(d)", NULL, KL_OVRP_POSEBOUND_MIXED,
      "OpenXR frames with ovrp_Update2(0, 0) in mid-frame (Walkabout)",
      "the Update2 does not change the head inside the frame, nothing frozen" },
};

// Knobs that would change what the pattern reads, were one left over in the
// caller's environment: a forced IPD replaces the eye offsets the gate pushes,
// the latch and the record switches select other paths, a scripted head or a
// test yaw moves the pose.
static const char *const CLEARED[] = {
    "KL_OVRP_POSE_BOUNDARY", "KL_OVRP_LATCH", "KL_OVRP_IPD",
    "KL_OVRP_EYE_CANT", "KL_OVRP_HEAD", "KL_OVRP_RECORD_INDEXED",
    "KL_OVRP_RECORD_LATE", "KL_OVRP_TEST_BR_YAW", "KL_OVRP_POSE_TRACE",
};

static int run(unsigned i) {
    for (unsigned k = 0; k < sizeof CLEARED / sizeof CLEARED[0]; k++)
        unsetenv(CLEARED[k]);
    if (CASES[i].boundary) setenv("KL_OVRP_POSE_BOUNDARY", CASES[i].boundary, 1);
    printf("--- %s %s ---\n", CASES[i].tag, CASES[i].title);
    int ok = kl_ovrp_pose_boundary_selftest(stdout, CASES[i].which);
    printf("%s %s: %s\n", ok ? "PASS" : "FAIL", CASES[i].tag, CASES[i].verdict);
    fflush(stdout);
    return ok;
}

int main(void) {
    printf("=== one head pose per guest frame ===\n");
    int all = 1;
    for (unsigned i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        fflush(stdout);
        fflush(stderr);
        pid_t pid = fork();
        if (pid == 0) _exit(run(i) ? 0 : 1);
        if (pid < 0) {
            printf("FAIL: fork() failed — this gate needs a process per case\n");
            return 1;
        }
        int st = 0;
        if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            // A child that died says nothing itself.
            if (!WIFEXITED(st))
                printf("FAIL %s: the case did not run to its end\n", CASES[i].tag);
            all = 0;
        }
    }
    printf("%s: the pose boundary — the frame cycle, its index mode, and inert "
           "for a guest that does not delimit its frames\n",
           all ? "PASS" : "FAIL");
    return all ? 0 : 1;
}
