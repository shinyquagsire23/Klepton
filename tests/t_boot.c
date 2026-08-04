// M3 gate: enter the guest through its real entry point.
//
// The plan said this milestone ends at ANativeActivity_onCreate. It does not —
// there is no NativeActivity in this APK. AndroidManifest.xml declares
// com.unity3d.player.UnityPlayerActivity, and libmain.so exports exactly one
// thing: JNI_OnLoad. So the true entry is a JNI call, and the gate is that
// libmain's JNI_OnLoad runs against our synthetic JavaVM and registers the two
// natives the Java side expects.
//
// Phase 1 is the assertion. Phase 2 is reconnaissance: it drives the registered
// NativeLoader.load() to find out what the *next* milestone has to answer for,
// and runs in a forked child because an unimplemented JNI slot aborts by design.
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../runtime/klepton.h"
#include "../runtime/kl_jni.h"
#include "../runtime/kl_egl.h"
#include "../runtime/kl_opensl.h"

static const char *LIBDIR = "beatsaber/lib/arm64-v8a";

typedef int  (*jni_onload_fn)(void *vm, void *reserved);
typedef int8_t (*nativeloader_load_fn)(void *env, void *clazz, void *path);

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

// A fault in guest code is the one stop the parent can only name by number, and
// by then the child is gone. Reporting it from inside says the three things that
// identify it: where it touched, where it was executing, and — the question a
// signal number cannot answer — whether it was even the main thread. A crash on
// an engine worker lands at whatever point the main thread's log had reached,
// which reads as a different bug on every run.
//
// This has to survive being called in a broken process, so it uses write(2)
// rather than stdio and does not attempt a symbolised backtrace.
static void report_fault(int sig, siginfo_t *si, void *uctx) {
    // Our own handler, so this is a Darwin ucontext_t and reading it is safe —
    // the mismatch in trap 5 is about the *guest's* handlers seeing one.
    ucontext_t *uc = uctx;
    void *pc = uc ? (void *)uc->uc_mcontext->__ss.__pc : NULL;
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);

    size_t      off = 0;
    const char *img = kl_addr_image(pc, &off);

    char buf[256];
    int  n = snprintf(buf, sizeof buf,
                      "\n[t_boot] fault: signal %d at %p, pc %s+0x%zx (%p), thread %llu%s\n",
                      sig, si ? si->si_addr : NULL, img ? img : "<host>", off, pc,
                      (unsigned long long)tid,
                      pthread_main_np() ? " (main)" : " (a guest worker thread)");
    if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }

    // SIGABRT here is usually the *guest* dying, not us — Unity's audio thread
    // aborts outright when FMOD cannot open an output device. Those paths never
    // touch kl_fatal_prepare(), so this is the only place the graphics surface
    // report can still be emitted. Not async-signal-safe, but nothing after this
    // point is going to run anyway.
    // SIGALRM means the guest is still alive and blocked, which is a different
    // question from a crash — the surface reports say how far it got and what it
    // was waiting on, so emit them here too.
    if (sig == SIGABRT || sig == SIGALRM) {
        kl_egl_report(stderr);
        kl_opensl_report(stderr);
    }

    // Die of the original signal, so the parent still reports it as one.
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_fault_reporter(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = report_fault;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
}

int main(int argc, char **argv) {
    if (argc > 1) LIBDIR = argv[1];
    kl_set_library_path(LIBDIR);

    char path[1024];
    snprintf(path, sizeof path, "%s/libmain.so", LIBDIR);

    printf("=== libmain.so entry ===\n");
    kl_image *main_img = kl_load(path);
    if (!main_img) return fail(kl_error());
    kl_register_image("libmain.so", main_img);
    kl_run_init(main_img);

    jni_onload_fn onload = (jni_onload_fn)kl_sym(main_img, "JNI_OnLoad");
    if (!onload) return fail("libmain.so exports no JNI_OnLoad");

    int version = onload(kl_jni_vm(), NULL);
    printf("  JNI_OnLoad returned 0x%08x\n", version);
    if (version != KL_JNI_VERSION_1_6)
        return fail("JNI_OnLoad did not return JNI_VERSION_1_6");

    // libmain registers com.unity3d.player.NativeLoader.{load,unload} — the
    // shim Unity's Java side calls to dlopen libunity.so.
    const char *CLS = "com/unity3d/player/NativeLoader";
    void *load   = kl_jni_native(CLS, "load", NULL);
    void *unload = kl_jni_native(CLS, "unload", NULL);
    if (!load || !unload) return fail("NativeLoader natives were not registered");
    printf("  registered %s.load=%p unload=%p\n", CLS, load, unload);

    printf("\n=== M3 EXIT CRITERION MET: guest JNI_OnLoad ran, natives registered ===\n");

    // ---- phase 2: reconnaissance, non-fatal ----
    // Unimplemented JNI slots abort the process on purpose, so this runs in a
    // child. Whatever it prints before dying is the M4 work list.
    printf("\n=== recon: driving NativeLoader.load(\"libunity.so\") ===\n");
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        install_fault_reporter();
        // Strict: an unimplemented *call* is fatal. Lookups are not, so this
        // stops only where the surface genuinely ends. KL_PERMISSIVE=1 flips it
        // to a zero return, which collects a whole batch in one run when pushing
        // into new territory — scouting only, since the guest then carries on
        // with answers we made up.
        kl_jni_set_permissive(getenv("KL_PERMISSIVE") != NULL);
        // load() takes the *directory* — it appends "/libunity.so" itself.
        int8_t ok = ((nativeloader_load_fn)load)(kl_jni_env(), NULL,
                                                 kl_jni_new_string(LIBDIR));
        printf("  NativeLoader.load returned %d\n", ok);

        // UnityPlayer's constructor calls initJni(Context) first (UnityPlayer.smali
        // line 372). It is `private final native`, so an instance method: the guest
        // sees (JNIEnv*, jobject thiz, jobject context). Both objects are opaque to
        // us — what matters is what libunity asks them for.
        void *initJni = kl_jni_native("com/unity3d/player/UnityPlayer", "initJni", NULL);
        if (initJni) {
            printf("\n=== recon: UnityPlayer.initJni(Context) ===\n");
            fflush(NULL);
            void *thiz = kl_jni_new_object("com/unity3d/player/UnityPlayer");
            // On device the Context is the Activity — AndroidManifest.xml declares
            // UnityPlayerActivity — and Unity checks that with IsInstanceOf. Handing
            // it a bare Context would send it down the no-Activity path.
            void *context = kl_jni_new_object("com/unity3d/player/UnityPlayerActivity");
            ((void (*)(void *, void *, void *))initJni)(kl_jni_env(), thiz, context);
            printf("  initJni returned\n");
        }

        // Lifecycle, in the order UnityPlayerActivity drives it: attach a
        // surface, resume, then pump one frame. This is where M4 runs into M5 —
        // nativeRecreateGfxState is what reaches for EGL.
        if (getenv("KL_LIFECYCLE")) {
            void *surface = kl_jni_new_object("android/view/Surface");
            void *thiz2   = kl_jni_new_object("com/unity3d/player/UnityPlayer");
            struct { const char *name; int kind; } seq[] = {
                {"nativeRecreateGfxState", 2}, {"nativeResume", 0}, {"nativeRender", 1},
            };
            for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
                void *fn = kl_jni_native("com/unity3d/player/UnityPlayer", seq[i].name, NULL);
                if (!fn) { printf("  %s: not registered\n", seq[i].name); continue; }
                printf("\n=== recon: UnityPlayer.%s ===\n", seq[i].name);
                fflush(NULL);
                // The render loop may block; do not hang the sweep. KL_ALARM
                // widens the window when the question is what it is waiting on.
                const char *aenv = getenv("KL_ALARM");
                alarm(aenv ? (unsigned)strtoul(aenv, NULL, 10) : 20);
                if (seq[i].kind == 2)
                    ((void (*)(void *, void *, int, void *))fn)(kl_jni_env(), thiz2, 0, surface);
                else if (seq[i].kind == 1)
                    printf("  -> %d\n", ((int8_t (*)(void *, void *))fn)(kl_jni_env(), thiz2));
                else
                    ((void (*)(void *, void *))fn)(kl_jni_env(), thiz2);
                alarm(0);
                printf("  %s returned\n", seq[i].name);
            }
        }

        kl_jni_report(stdout);
        kl_egl_report(stdout);
        fflush(NULL);   // _exit does not flush stdio, and the report is the point
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st) || WEXITSTATUS(st) != 0) {
        // How it stopped is the first question every time: an unimplemented JNI
        // slot aborts (SIGABRT after the report), SIGALRM means the guest
        // blocked, and anything else is a real fault in guest code.
        if (WIFSIGNALED(st))
            printf("\n  (recon stopped on signal %d — %s)\n", WTERMSIG(st),
                   strsignal(WTERMSIG(st)));
        else
            printf("\n  (recon child exited %d)\n", WEXITSTATUS(st));
        printf("  (see the JNI surface report above)\n");
        return fail("guest init did not complete: an unimplemented JNI call was reached");
    }
    printf("\n=== M4 (partial): initJni completed with no unimplemented JNI calls ===\n");
    return 0;
}
