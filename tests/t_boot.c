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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../runtime/klepton.h"
#include "../runtime/kl_jni.h"

static const char *LIBDIR = "beatsaber/lib/arm64-v8a";

typedef int  (*jni_onload_fn)(void *vm, void *reserved);
typedef int8_t (*nativeloader_load_fn)(void *env, void *clazz, void *path);

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

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
        // Strict: an unimplemented *call* is fatal. Lookups are not, so this
        // stops only where the surface genuinely ends. Flip to permissive when
        // pushing into new territory and you want the whole batch in one run.
        kl_jni_set_permissive(0);
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

        kl_jni_report(stdout);
        fflush(NULL);   // _exit does not flush stdio, and the report is the point
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st) || WEXITSTATUS(st) != 0) {
        printf("\n  (recon stopped — see the JNI surface report above)\n");
        return fail("guest init did not complete: an unimplemented JNI call was reached");
    }
    printf("\n=== M4 (partial): initJni completed with no unimplemented JNI calls ===\n");
    return 0;
}
