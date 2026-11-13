// UE's virtual keyboard gate. `make uevk`, in `make check`.
//
// The complement of `make softinput`: that one gates Unity's on-screen
// keyboard, this one gates Unreal's, and the two share a frontend contract but
// no code at all. The check itself lives in runtime/jni/kl_jni_ue4.c for the
// same reason the soft-input one lives beside its bindings — what it asserts is
// that file's internal model (the report queue, the two close shapes read out
// of GameActivity$p0/$q0 and y0.onKeyPreIme, what a host with no keyboard does
// instead of nothing), and the pieces it drives are the real bindings the guest
// calls.
//
// What this file supplies is the environment and the exit code.
#include <stdio.h>
#include "../runtime/kl_jni.h"

int main(void) {
    printf("=== UE's virtual keyboard (GameActivity) ===\n");
    int ok = kl_jni_ue_keyboard_selftest(stdout);
    printf("%s: the virtual keyboard reports what the guest's own Java would, "
           "and a host with no keyboard ENDS the entry — result(true) before "
           "anything resets the widget, not a hide the widget never hears\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
