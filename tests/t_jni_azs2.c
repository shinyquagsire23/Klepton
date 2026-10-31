// Arizona Sunshine 2's two JNI answers: Vertigo's LogcatUtility.readLogs hands
// back an empty string, and the Sentry Java SDK (io/sentry/...) is dropped —
// calls and field reads answer zero/NULL, field writes are not kept. Both for
// that title only: any other target still stops on the name. Each case runs in
// a child of its own, because the switches are read once per process.
// No SDK, game or network needed.
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

#define SLOT(n, name) JNI_##name = n,
enum { KL_JNI_SLOTS(SLOT) };
#undef SLOT
#define JNI(name, type) ((type)jni[JNI_##name])
#define AZS2 "x5245041552210029"
#define LOGCAT "com/vertigogames/vertigoandroidutils/LogcatUtility"
static void *env;
static void **jni;

static void start(const char *target, int permissive) {
    static kl_target t;
    t.name = target;
    kl_driver_init(&t, NULL, 0);
    env = kl_jni_env(); jni = *(void ***)env;
    kl_jni_set_permissive(permissive);
}

static void *read_logs(void) {
    void *cls = kl_jni_class(LOGCAT);
    void *m = JNI(GetStaticMethodID, void *(*)(void *, void *, const char *, const char *))
        (env, cls, "readLogs", "(Ljava/lang/String;)Ljava/lang/String;");
    uint64_t arg = (uintptr_t)kl_jni_new_string("-t Unity");
    kl_va a = {.gr_top = &arg + 1, .gr_offs = -8};
    return JNI(CallStaticObjectMethodV, void *(*)(void *, void *, void *, kl_va *))(env, cls, m, &a);
}

static void *current_hub(void) {
    void *cls = kl_jni_class("io/sentry/Sentry");
    void *m = JNI(GetStaticMethodID, void *(*)(void *, void *, const char *, const char *))
        (env, cls, "getCurrentHub", "()Lio/sentry/IHub;");
    kl_va none = {0};
    return JNI(CallStaticObjectMethodV, void *(*)(void *, void *, void *, kl_va *))(env, cls, m, &none);
}

// More Breadcrumbs than the 128-entry field-write table holds, each written and
// read back. Returns how many reads gave back what was written.
static int breadcrumbs(int n) {
    void *cls = kl_jni_class("io/sentry/Breadcrumb");
    void *f = JNI(GetFieldID, void *(*)(void *, void *, const char *, const char *))
        (env, cls, "message", "Ljava/lang/String;");
    int kept = 0;
    for (int i = 0; i < n; i++) {
        kl_jni_local_frame_push();
        void *b = kl_jni_new_object("io/sentry/Breadcrumb");
        void *s = kl_jni_new_string("loaded");
        JNI(SetObjectField, void (*)(void *, void *, void *, void *))(env, b, f, s);
        if (JNI(GetObjectField, void *(*)(void *, void *, void *))(env, b, f) == s) kept++;
        kl_jni_local_frame_pop();
    }
    return kept;
}

static void *sentry_level(void) {
    void *cls = kl_jni_class("io/sentry/SentryLevel");
    void *f = JNI(GetStaticFieldID, void *(*)(void *, void *, const char *, const char *))
        (env, cls, "INFO", "Lio/sentry/SentryLevel;");
    return JNI(GetStaticObjectField, void *(*)(void *, void *, void *))(env, cls, f);
}

// The title itself, with the run's own stop rules (not permissive).
static int target_answers(void) {
    start(AZS2, 0);
    kl_jni_local_frame_push();
    void *s = read_logs();
    assert(s);
    assert(JNI(GetStringUTFLength, int (*)(void *, void *))(env, s) == 0);
    kl_jni_local_frame_pop();
    for (int i = 0; i < 3; i++) assert(current_hub() == NULL);
    assert(sentry_level() == NULL);
    assert(breadcrumbs(300) == 0);   // dropped, and never fills the table
    return 0;
}
// Any other title in permissive mode: Sentry writes are kept as before.
static int other_keeps_writes(void) {
    start("jni-azs2-test-other", 1);
    assert(breadcrumbs(8) == 8);
    return 0;
}
static int other_sentry_call(void)   { start("jni-azs2-test-other", 0); current_hub(); return 0; }
static int other_sentry_field(void)  { start("jni-azs2-test-other", 0); sentry_level(); return 0; }
static int other_read_logs(void)     { start("jni-azs2-test-other", 0); read_logs(); return 0; }
static int target_switched_off(void) { setenv("KL_JNI_DROP_SENTRY", "0", 1); start(AZS2, 0); current_hub(); return 0; }

// 0: the child returned 0; SIGABRT: it stopped by name; anything else fails.
static int run(int (*fn)(void)) {
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        // The expected aborts would otherwise each leave a crash report.
        signal(SIGABRT, SIG_DFL);
        _exit(fn());
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (WIFEXITED(st)) return WEXITSTATUS(st) ? -1 : 0;
    return WIFSIGNALED(st) ? WTERMSIG(st) : -1;
}

int main(void) {
    int bad = 0;
#define EXPECT(fn, want) do { int r = run(fn); if (r != (want)) { \
        fprintf(stderr, "FAIL: %s gave %d, want %d\n", #fn, r, (want)); bad = 1; } } while (0)
    EXPECT(target_answers, 0);
    EXPECT(other_keeps_writes, 0);
    EXPECT(other_sentry_call, SIGABRT);
    EXPECT(other_sentry_field, SIGABRT);
    EXPECT(other_read_logs, SIGABRT);
    EXPECT(target_switched_off, SIGABRT);
#undef EXPECT
    if (bad) return 1;
    puts("PASS: Arizona Sunshine 2 reads an empty logcat and drops the Sentry Java SDK "
         "(calls, field reads, unkept writes past the table size); other titles and "
         "KL_JNI_DROP_SENTRY=0 still stop on the name.");
    return 0;
}
