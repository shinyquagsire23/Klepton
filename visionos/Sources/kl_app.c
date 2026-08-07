// See kl_app.h.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "kl_egl.h"

static char g_libdir[1024];
static char g_assets[1024];
static char g_files[1024];
static char g_apk[1024];
static char g_dylibs[1024];
static char g_log[1024];
static char g_status[512] = "not configured";

const char *kl_app_log_path(void) { return g_log; }
const char *kl_app_status(void)   { return g_status; }

static int have(const char *p) { struct stat st; return p && *p && stat(p, &st) == 0; }

// Frameworks/ always exists (the Swift runtime lives there), so its presence
// says nothing. What matters is whether *our* translations were embedded.
static int have_translations(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/libmain.framework/libmain", g_dylibs);
    return have(p);
}

static int missing(const char *what, const char *path) {
    snprintf(g_status, sizeof g_status, "missing %s: %s", what, path);
    return 1;
}

int kl_app_configure(const char *resources, const char *container) {
    if (!resources || !container) return missing("path", "(null)");

    // The guest libraries ride in the bundle: AMFI is content about a dylib
    // inside a bundle we signed (P3/P12), and nothing has established that it
    // is content about one pushed into Documents afterwards. The 2.3 GB of
    // assets go the other way — into the container — because they carry no
    // code and re-uploading them on every install would make the M4 loop
    // unusable. See visionos/README.md.
    snprintf(g_libdir, sizeof g_libdir, "%s/guest/lib/arm64-v8a", resources);
    // Frameworks/, not a directory of our own: that is where Xcode code-signs
    // what it embeds, and a loose Mach-O elsewhere in the bundle is only
    // sealed. kl_load_auto knows both layouts.
    snprintf(g_dylibs, sizeof g_dylibs, "%s/Frameworks", resources);
    snprintf(g_assets, sizeof g_assets, "%s/beatsaber/assets", container);
    snprintf(g_apk,    sizeof g_apk,    "%s/beatsaber.apk", container);
    snprintf(g_files,  sizeof g_files,  "%s/android-files", container);
    snprintf(g_log,    sizeof g_log,    "%s/klepton-boot.log", container);

    // Check before running, not after failing. A missing asset tree otherwise
    // surfaces three layers up inside Unity as something that reads like a shim
    // bug — trap 6c is exactly that failure wearing a different hat.
    // With translations embedded, g_libdir never has to exist: it is only the
    // string NativeLoader.load() is handed and the prefix the DT_NEEDED walk
    // builds paths from, and kl_load_auto turns "<libdir>/libunity.so" into
    // "Frameworks/libunity.framework/libunity" before touching the disk. So the
    // ELF tree is not in the bundle — 80 MB that would buy an A/B against the
    // mmap loader, which is the thing M1b exists to replace.
    if (!have_translations() && !have(g_libdir))
        return missing("guest libraries (neither translations nor an ELF tree)", g_libdir);
    if (!have(g_assets)) return missing("staged assets (run stage_assets.sh)", g_assets);
    if (!have(g_apk))    return missing("staged APK (run stage_assets.sh)", g_apk);
    mkdir(g_files, 0755);

    kl_set_library_path(g_libdir);
    kl_jni_set_assets_dir(g_assets);
    kl_jni_set_apk_path(g_apk);
    kl_jni_set_files_dir(g_files);
    // Raw "<apk>/assets/..." opens: Unity mounts the APK into its VFS and then
    // resolves entries by concatenating onto the mount point (trap 6c).
    kl_guest_path_map(g_apk, g_assets);

    // Prefer klepton-ld translations when the bundle carries them. On device
    // this is not a preference but the whole point — the mmap loader maps guest
    // text RWX from a file the bundle does not own, which is the shape AMFI
    // exists to refuse. A translation that is present and fails is an error,
    // not a fallback (kl_load_auto).
    if (have_translations()) setenv("KL_DYLIB_DIR", g_dylibs, 1);

    snprintf(g_status, sizeof g_status, "configured");
    return 0;
}

// Everything the run prints goes to a file, line-buffered.
//
// Line-buffered specifically: an unimplemented JNI slot aborts by design, and
// a fully-buffered stream loses the whole report when the process dies on a
// signal — which on the host reads as a much earlier failure than actually
// happened, and on device would be the only evidence there is.
static int open_log(void) {
    if (!freopen(g_log, "w", stdout)) return 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    dup2(fileno(stdout), STDERR_FILENO);   // the guest's own writes(2) land here too
    setvbuf(stderr, NULL, _IOLBF, 0);
    return 0;
}

typedef int    (*jni_onload_fn)(void *vm, void *reserved);
typedef int8_t (*nativeloader_load_fn)(void *env, void *clazz, void *path);

static int fail(const char *msg) {
    snprintf(g_status, sizeof g_status, "%s", msg);
    fprintf(stderr, "FAIL: %s\n", msg);
    fflush(NULL);
    return 1;
}

int kl_app_boot(void) {
    // Once, and never concurrently. The Boot button invites a second press,
    // and the second entry is not merely redundant: the runtime's JNI tables
    // are process-global, so another JNI_OnLoad re-registers every native onto
    // the same table, while open_log() truncates the file the first run is
    // still writing. Five presses on device produced "natives registered: 61"
    // instead of 45 with a chunk of the log missing — which reads as a
    // platform difference and is not one. Refusing is better than allowing a
    // run whose numbers cannot be compared to anything.
    static pthread_mutex_t once_mu = PTHREAD_MUTEX_INITIALIZER;
    static int entered;
    pthread_mutex_lock(&once_mu);
    int already = entered;
    entered = 1;
    pthread_mutex_unlock(&once_mu);
    if (already) return fail("kl_app_boot was already run in this process");

    if (!*g_libdir) return fail("kl_app_configure was not called");
    if (open_log()) return fail("could not open the log file");

    kl_fault_install();
    // Strict: an unimplemented *call* is fatal, so the run stops exactly where
    // the surface genuinely ends. Lookups are not — the guest resolves plenty
    // it never calls.
    kl_jni_set_permissive(getenv("KL_PERMISSIVE") != NULL);
    kl_egl_dump_textures(getenv("KL_DUMP_TEXTURES"));

    printf("=== Klepton on visionOS — P4 ===\n");
    printf("  libraries : %s\n", g_libdir);
    printf("  dylibs    : %s%s\n", g_dylibs,
           have_translations() ? "" : "  (none embedded — falling back to the mmap ELF loader)");
    printf("  assets    : %s\n", g_assets);
    printf("  apk       : %s\n", g_apk);
    printf("  files     : %s\n\n", g_files);
    fflush(NULL);

    char path[1200];
    snprintf(path, sizeof path, "%s/libmain.so", g_libdir);

    printf("=== libmain.so entry ===\n");
    kl_image *main_img = kl_load_auto(path);
    if (!main_img) return fail(kl_error());
    kl_register_image("libmain.so", main_img);
    kl_run_init(main_img);

    jni_onload_fn onload = (jni_onload_fn)kl_sym(main_img, "JNI_OnLoad");
    if (!onload) return fail("libmain.so exports no JNI_OnLoad");

    kl_jni_local_frame_push();          // the JVM would pop each native's local
    int version = onload(kl_jni_vm(), NULL);  // frame on return; the host plays
    kl_jni_local_frame_pop();           // that half (see kl_jni.h)
    printf("  JNI_OnLoad returned 0x%08x\n", version);
    if (version != KL_JNI_VERSION_1_6)
        return fail("JNI_OnLoad did not return JNI_VERSION_1_6");

    const char *CLS = "com/unity3d/player/NativeLoader";
    void *load = kl_jni_native(CLS, "load", NULL);
    if (!load || !kl_jni_native(CLS, "unload", NULL))
        return fail("NativeLoader natives were not registered");
    printf("  registered %s.load=%p\n", CLS, load);
    printf("\n=== M3 EXIT CRITERION MET: guest JNI_OnLoad ran, natives registered ===\n");
    fflush(NULL);

    // load() takes the *directory* — it appends "/libunity.so" itself.
    printf("\n=== NativeLoader.load(\"%s\") ===\n", g_libdir);
    fflush(NULL);
    kl_jni_local_frame_push();
    int8_t ok = ((nativeloader_load_fn)load)(kl_jni_env(), NULL,
                                             kl_jni_new_string(g_libdir));
    kl_jni_local_frame_pop();
    printf("  NativeLoader.load returned %d\n", ok);
    if (!ok) return fail("NativeLoader.load could not bring up libunity.so");

    // UnityPlayer's constructor calls initJni(Context) first. It is
    // `private final native`, so the guest sees (JNIEnv*, jobject thiz,
    // jobject context). The Context must be the Activity — the manifest
    // declares UnityPlayerActivity and Unity checks with IsInstanceOf — and the
    // shared singleton, because Unity reads it back through the static
    // UnityPlayer.currentActivity and compares.
    void *initJni = kl_jni_native("com/unity3d/player/UnityPlayer", "initJni", NULL);
    if (!initJni) return fail("UnityPlayer.initJni was never registered");
    printf("\n=== UnityPlayer.initJni(Context) ===\n");
    fflush(NULL);
    void *thiz = kl_jni_new_object("com/unity3d/player/UnityPlayer");
    kl_jni_local_frame_push();
    ((void (*)(void *, void *, void *))initJni)(kl_jni_env(), thiz, kl_jni_activity());
    kl_jni_local_frame_pop();
    printf("  initJni returned\n");

    printf("\n=== P4 EXIT CRITERION MET: initJni completed on visionOS, "
           "no unimplemented JNI calls ===\n");
    kl_jni_report(stdout);
    fflush(NULL);
    snprintf(g_status, sizeof g_status, "initJni completed");
    return 0;
}
