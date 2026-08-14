// The on-screen keyboard gate. `make softinput`, in `make check`.
//
// The check itself lives in kl_jni.c (kl_jni_soft_input_selftest) for the same
// reason `make xrinput`'s lives in kl_openxr.c: what it asserts is that file's
// internal model — the report queue, the order UnityPlayer$i defines, the
// character limit, what a cancellation withholds — and the pieces it drives are
// the real bindings the guest calls. Testing a copy of them would test the copy.
//
// What this file supplies is the environment and the exit code.
#include <stdio.h>
#include "../runtime/kl_jni.h"

int main(void) {
    printf("=== the on-screen keyboard (Unity soft input) ===\n");
    int ok = kl_jni_soft_input_selftest(stdout);
    printf("%s: the soft input reports what the guest's own Java would\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
