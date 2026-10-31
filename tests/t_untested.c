// The profile for untested games (kl_untested_profile, runtime/guest/kl_driver.c).
// `make untested`, in `make check`.
//
// Which titles it applies to, the OVRPlugin version it reads from a game's own
// libOVRPlugin.so, and that an unknown Java method then answers zero instead of
// ending the run. The profile is decided once a process (as in the shipping
// build), so the JNI cases are children forked before anything asked.
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kl_jni.h"
#include "kl_jni_slots.h"
#include "kl_va.h"
#include "guest/kl_driver.h"
#include "guest/kl_tuned.h"
#include "xr/kl_ovrp.h"

#define SLOT(n, name) JNI_##name = n,
enum { KL_JNI_SLOTS(SLOT) };
#undef SLOT
#define JNI(name, type) ((type)jni[JNI_##name])

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static int version_of(const char *bytes, size_t n, char *out) {
    char path[] = "/tmp/t_untested.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    write(fd, bytes, n);
    close(fd);
    int r = kl_ovrp_guest_version_in(path, out, 24);
    unlink(path);
    return r;
}

// A child: the target, then one unknown static method of each return kind.
// Exits 0 if every call came back.
static int unknown_calls(const char *target) {
    const kl_target t = {.name = target};
    kl_driver_init(&t, NULL, 0);
    void *env = kl_jni_env();
    void **jni = *(void ***)env;
    void *cls = kl_jni_class("com/example/untested/Vendor");
    void *mv = JNI(GetStaticMethodID, void *(*)(void *, void *, const char *, const char *))
        (env, cls, "start", "(Ljava/lang/String;)V");
    void *mi = JNI(GetStaticMethodID, void *(*)(void *, void *, const char *, const char *))
        (env, cls, "level", "()I");
    void *mo = JNI(GetStaticMethodID, void *(*)(void *, void *, const char *, const char *))
        (env, cls, "session", "()Ljava/lang/Object;");
    uint64_t arg = 0;
    kl_va a = {.gr_top = &arg + 1, .gr_offs = -8};
    JNI(CallStaticVoidMethodV, void (*)(void *, void *, void *, kl_va *))(env, cls, mv, &a);
    kl_va b = {.gr_top = &arg + 1, .gr_offs = -8};
    int32_t i = JNI(CallStaticIntMethodV, int32_t (*)(void *, void *, void *, kl_va *))(env, cls, mi, &b);
    kl_va c = {.gr_top = &arg + 1, .gr_offs = -8};
    void *o = JNI(CallStaticObjectMethodV, void *(*)(void *, void *, void *, kl_va *))(env, cls, mo, &c);
    return i == 0 && o == NULL ? 0 : 3;
}

static int run_child(const char *target, const char *untested_env) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (untested_env) setenv("KL_UNTESTED", untested_env, 1);
        freopen("/dev/null", "w", stderr);
        _exit(unknown_calls(target));
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

int main(void) {
    printf("=== the profile for untested games ===\n");

    // Which titles: a store title nobody has tested, and nothing else.
    unsetenv("KL_UNTESTED");
    ck(kl_untested_target("x1234567890123456") == 1, "an untested store title is under the profile");
    ck(kl_untested_target("x9531494193591052") == 0, "a title brought up before (Ghost Town) is not");
    ck(kl_untested_target("beatsaber") == 0, "a named target is not");
    ck(kl_untested_target(NULL) == 0, "no target is not");
    setenv("KL_UNTESTED", "0", 1);
    ck(kl_untested_target("x1234567890123456") == 0, "KL_UNTESTED=0 turns it off");
    setenv("KL_UNTESTED", "1", 1);
    ck(kl_untested_target("beatsaber") == 1, "KL_UNTESTED=1 turns it on for any target");
    unsetenv("KL_UNTESTED");
    size_t n = sizeof kl_tuned_targets / sizeof kl_tuned_targets[0];
    int sorted = 1, found = 1;
    for (size_t k = 0; k < n; k++) {
        if (k && strcmp(kl_tuned_targets[k - 1], kl_tuned_targets[k]) >= 0) sorted = 0;
        if (!kl_target_tuned(kl_tuned_targets[k])) found = 0;
    }
    ck(sorted, "kl_tuned.h is sorted without duplicates (it is searched with bsearch)");
    ck(found, "every title in kl_tuned.h is found");

    // The OVRPlugin version a game ships.
    char v[24];
    static const char one[] = "\0junk\0" "1.5.5\0" "x\0" "1.108.0\0" "CompositorOpenXR.cpp";
    ck(version_of(one, sizeof one, v) == 1 && strcmp(v, "1.108.0") == 0,
       "the one 1.<minor 20 or more>.<patch> string is the version (1.5.5 beside it is not)");
    static const char twice[] = "\0" "1.101.1\0" "1.101.1\0";
    ck(version_of(twice, sizeof twice, v) == 1 && strcmp(v, "1.101.1") == 0, "the same one twice is still one");
    static const char two[] = "\0" "1.98.0\0" "1.109.0\0";
    ck(version_of(two, sizeof two, v) == 0 && !v[0], "two different ones are no answer");
    static const char glued[] = "\0" "v1.108.0\0" "1.108.0x\0";
    ck(version_of(glued, sizeof glued, v) == 0, "a number inside other text is not one");
    ck(kl_ovrp_guest_version_in("/nonexistent/libOVRPlugin.so", v, sizeof v) == 0, "no library is no answer");
    // A real one, when given: T_UNTESTED_OVRP=<path to a libOVRPlugin.so>=<version>.
    const char *real = getenv("T_UNTESTED_OVRP");
    if (real && strchr(real, '=')) {
        char path[1024]; snprintf(path, sizeof path, "%s", real);
        char *want = strrchr(path, '='); *want++ = 0;
        ck(kl_ovrp_guest_version_in(path, v, sizeof v) == 1 && strcmp(v, want) == 0,
           "a game's own libOVRPlugin.so names its version");
    }

    // An unknown Java method: zero under the profile, the stop by name without.
    int st = run_child("x1234567890123456", NULL);
    ck(WIFEXITED(st) && WEXITSTATUS(st) == 0,
       "under the profile, unknown void, int and object methods answer nothing, 0 and null");
    st = run_child("x9531494193591052", NULL);
    ck(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT, "a tested title still stops by name on the first one");
    st = run_child("x1234567890123456", "0");
    ck(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT, "...and so does an untested one with KL_UNTESTED=0");

    printf("untested: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
