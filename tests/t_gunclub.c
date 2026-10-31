// Gun Club VR's quit and preference-file trace (fix 44), and that every other
// title is left as it was. `make gunclub`, in `make check`.
//
// The quit: Unity's lifecycle natives are registered as fakes and the real
// kl_driver_frame drives them. For Gun Club the first false from nativeRender
// is followed by nativePause, then nativeDone, once, and nativeRender is never
// called again. Any other title, and Gun Club with KL_UNITY_QUIT_TEARDOWN=0,
// gets no pause and no teardown and goes on rendering.
//
// The trace: kl_fs_trace_open with KL_TRACE_FS unset prints nothing for
// another title; for Gun Club it prints the TBMPrefs lines (flags, descriptor)
// and nothing else. In every case the failure's errno is what the caller sees
// afterwards. Each case is a child of its own, since the switches are read
// once per process.
//
// The eye size: for Gun Club the display's request is capped at 4000 on its
// longer axis instead of the general 3072; any other title keeps 3072, and
// KL_OVRP_EYE_MAX and the render scale still apply on top. No game content.
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kl_jni.h"
#include "kl_jni_slots.h"
#include "klepton.h"
#include "guest/kl_driver.h"
#include "diag/kl_mprobe.h"
#include "xr/kl_ovrp.h"

#define SLOT(n, name) JNI_##name = n,
enum { KL_JNI_SLOTS(SLOT) };
#undef SLOT
#define JNI(name, type) ((type)jni[JNI_##name])
#define PLAYER "com/unity3d/player/UnityPlayer"
#define GUNCLUB "x2285803881494677"
#define OTHER "gunclub-test-other"

static void *env;
static void **jni;

// ---- the quit ----
static int renders, false_at;   // nativeRender answers false from call false_at on
static char order[16];
static unsigned n_order;
static int8_t fake_render(void *e, void *thiz) { (void)e; (void)thiz; return ++renders < false_at; }
static int8_t fake_pause(void *e, void *thiz) { (void)e; (void)thiz; order[n_order++] = 'P'; return 1; }
static void   fake_done(void *e, void *thiz)  { (void)e; (void)thiz; order[n_order++] = 'D'; }

static void reg(const char *name, const char *sig, void *fn) {
    kl_jni_method m = {name, sig, fn};
    assert(JNI(RegisterNatives, kl_jint (*)(void *, void *, const kl_jni_method *, kl_jint))
        (env, kl_jni_class(PLAYER), &m, 1) == 0);
}

static void start(const char *target) {
    static kl_target t;
    t.name = target;
    kl_driver_init(&t, NULL, 0);
    kl_driver_set_alarm(0);
    env = kl_jni_env(); jni = *(void ***)env;
}

// Boot the Unity lifecycle on the fakes: its own first nativeRender answers
// true, the frame loop's first answers true, its second false.
static void begin_with_fakes(const char *target) {
    start(target);
    reg("nativeRender", "()Z", (void *)fake_render);
    reg("nativePause", "()Z", (void *)fake_pause);
    reg("nativeDone", "()V", (void *)fake_done);
    false_at = 3;
    FILE *out = fopen("/dev/null", "w");
    assert(kl_driver_lifecycle_begin(out) == 0);
    fclose(out);
    assert(renders == 1);
    assert(kl_driver_frame() == 1);
}

static int gunclub_quit_tears_down(void) {
    begin_with_fakes(GUNCLUB);
    assert(kl_driver_frame() == 0);             // the game's quit
    assert(n_order == 2 && order[0] == 'P' && order[1] == 'D');
    assert(kl_driver_frame() == -1);            // the engine is never called again
    assert(kl_driver_frame() == -1);
    assert(renders == 3 && n_order == 2);
    return 0;
}
static int gunclub_switched_off(void) {
    setenv("KL_UNITY_QUIT_TEARDOWN", "0", 1);
    begin_with_fakes(GUNCLUB);
    assert(kl_driver_frame() == 0);
    assert(kl_driver_frame() == 0);             // rendering goes on, as before
    assert(renders == 4 && n_order == 0);
    return 0;
}
static int other_unchanged(void) {
    begin_with_fakes(OTHER);
    assert(kl_driver_frame() == 0);
    assert(kl_driver_frame() == 0);
    assert(renders == 4 && n_order == 0);
    kl_mprobe_gunclub_save_preferences();       // another title: returns at once
    return 0;
}
static int other_switched_on(void) {
    // The teardown itself is generic; only TBMPrefs.Save is Gun Club's, and
    // it declines for any other target.
    setenv("KL_UNITY_QUIT_TEARDOWN", "1", 1);
    begin_with_fakes(OTHER);
    assert(kl_driver_frame() == 0);
    assert(n_order == 2 && order[0] == 'P' && order[1] == 'D');
    assert(kl_driver_frame() == -1);
    return 0;
}

// ---- the trace ----
// stderr into a file for the length of fn, and that file's text back.
static char captured[4096];
static void capture(void (*fn)(void)) {
    char path[] = "/tmp/t_gunclub.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    fflush(stderr);
    int saved = dup(2);
    dup2(fd, 2);
    fn();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, captured, sizeof captured - 1);
    captured[n > 0 ? n : 0] = 0;
    close(fd);
    unlink(path);
}

static void opens(void) {
    errno = ENOENT;
    kl_fs_trace_open("/data/files/TBMPrefs.tmp", 0x241, -1);
    assert(errno == ENOENT);
    errno = EACCES;
    kl_fs_trace_open("/data/files/settings.cfg", 0, -1);
    assert(errno == EACCES);
    errno = 0;
    kl_fs_trace_open("/data/files/TBMPrefs", 0, 37);
}

static int trace_other_silent(void) {
    start(OTHER);
    capture(opens);
    assert(captured[0] == 0);
    return 0;
}
static int trace_gunclub_prefs_only(void) {
    start(GUNCLUB);
    capture(opens);
    assert(strstr(captured, "[fs] open(\"/data/files/TBMPrefs.tmp\", \"flags=0x241 fd=-1\") FAILED"));
    assert(strstr(captured, "[fs] open(\"/data/files/TBMPrefs\", \"flags=0x0 fd=37\") ok"));
    assert(!strstr(captured, "settings.cfg"));
    return 0;
}
static int trace_on_shows_flags(void) {
    setenv("KL_TRACE_FS", "1", 1);
    start(OTHER);
    capture(opens);
    assert(strstr(captured, "[fs] open(\"/data/files/settings.cfg\", \"flags=0x0 fd=-1\") FAILED"));
    return 0;
}

static int run(const char *what, int (*fn)(void)) {
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) _exit(fn());
    int st = 0;
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

// ---- the eye size ----
// What the guest is told for a 4633x3717 request (the display's own size).
static void eye_for(const char *target, int *w, int *h) {
    start(target);
    kl_ovrp_set_eye_texture_size(4633, 3717);
    kl_ovrp_eye_texture_size(w, h);
}
static int eye_gunclub(void) {
    int w, h; eye_for(GUNCLUB, &w, &h);
    if (w != 4000 || h != 3209) { fprintf(stderr, "gunclub eye %dx%d\n", w, h); exit(1); }
    return 0;
}
static int eye_other(void) {
    int w, h; eye_for(OTHER, &w, &h);
    if (w != 3072 || h != 2465) { fprintf(stderr, "other eye %dx%d\n", w, h); exit(1); }
    return 0;
}
static int eye_gunclub_env_cap(void) {
    setenv("KL_OVRP_EYE_MAX", "3072", 1);
    int w, h; eye_for(GUNCLUB, &w, &h);
    if (w != 3072 || h != 2465) { fprintf(stderr, "gunclub capped eye %dx%d\n", w, h); exit(1); }
    return 0;
}
static int eye_gunclub_scaled(void) {
    setenv("KL_OVRP_RENDER_SCALE", "0.8", 1);
    int w, h; eye_for(GUNCLUB, &w, &h);
    if (w != 3200 || h != 2568) { fprintf(stderr, "gunclub 80%% eye %dx%d\n", w, h); exit(1); }
    return 0;
}

int main(void) {
    printf("=== Gun Club VR: quit teardown, preference-file trace ===\n");
    int ok = 1;
    ok &= run("Gun Club: quit -> nativePause, nativeDone, no more nativeRender", gunclub_quit_tears_down);
    ok &= run("Gun Club, KL_UNITY_QUIT_TEARDOWN=0: renders on, no teardown", gunclub_switched_off);
    ok &= run("other title: renders on, no teardown, no save", other_unchanged);
    ok &= run("other title, KL_UNITY_QUIT_TEARDOWN=1: teardown without the save", other_switched_on);
    ok &= run("other title, KL_TRACE_FS unset: no trace, errno kept", trace_other_silent);
    ok &= run("Gun Club, KL_TRACE_FS unset: TBMPrefs lines only, errno kept", trace_gunclub_prefs_only);
    ok &= run("KL_TRACE_FS=1: open lines carry flags and fd", trace_on_shows_flags);
    ok &= run("Gun Club: eye capped at 4000 (4000x3209)", eye_gunclub);
    ok &= run("other title: eye capped at 3072 (3072x2465)", eye_other);
    ok &= run("Gun Club, KL_OVRP_EYE_MAX=3072: 3072x2465", eye_gunclub_env_cap);
    ok &= run("Gun Club at 80 %: 3200x2568", eye_gunclub_scaled);
    puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
