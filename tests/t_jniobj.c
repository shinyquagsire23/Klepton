// The JNI object-lifetime gate. `make jniobj`, in `make check`.
//
// The check itself lives in kl_jni.c (kl_jni_object_lifetime_selftest), for the
// same reason `make softinput`'s lives in kl_jni_softinput.c: what it asserts is
// that file's own object model — the pool, the local-frame pops, the pinning a
// host-side cache owes — and the pieces it drives are the real bindings the
// guest calls. Testing a copy of them would test the copy.
//
// What this file supplies is the environment and the exit code.
#include <stdio.h>
#include "../runtime/kl_jni.h"

int main(void) {
    printf("=== JNI object lifetime and erased call spellings ===\n");
    int ok = kl_jni_object_lifetime_selftest(stdout);
    printf("%s: a cached jobject outlives its frame and every spelling of a "
           "call answers with it\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
