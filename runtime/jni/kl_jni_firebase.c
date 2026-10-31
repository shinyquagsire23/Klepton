// BattleGroupVR2's Firebase start, as a failure the SDK can report (fix 38,
// sent in by a user, 2026-10-13). This is NOT a Firebase port: there is no
// FirebaseApp and no Google Play service on this host, and nothing here says
// there is. The calls the SDK makes on its way to finding that out are
// answered with the exception Java would throw, so the SDK reports a missing
// backend to managed code instead of the process stopping on the first name.
// Nothing is sent anywhere; the library versions it registers go to the log.
//
// For that one title (x9386827918099701): what another game does once its
// Firebase calls throw instead of stopping it has not been looked at. Every
// other Firebase call stops by name, for this title too.
#include <pthread.h>
#include <string.h>
#include "guest/kl_driver.h"
#include "kl_jni_int.h"

#define FB_REG "com/google/firebase/platforminfo/GlobalLibraryVersionRegistrar"
#define FB_APP "com/google/firebase/FirebaseApp"
#define FB_OPTIONS "com/google/firebase/FirebaseOptions"
#define FB_LOG "com/google/firebase/app/internal/cpp/Log"
#define FB_GOOGLE_HELPER "com/google/firebase/app/internal/cpp/GoogleApiAvailabilityHelper"
#define GOOGLE_API "com/google/android/gms/common/GoogleApiAvailability"
#define GAMESTOTY "com/Gamestoty/GamestotyJava"

static klj_val registrar(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static void *instance;
    pthread_mutex_lock(&lock);
    klj_val result = klj_singleton(FB_REG, &instance);
    pthread_mutex_unlock(&lock);
    return result;
}
static klj_val register_version(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *library = n > 0 ? klj_str(a[0].l) : NULL;
    const char *version = n > 1 ? klj_str(a[1].l) : NULL;
    if (!library || !version) {
        klj_raise("java/lang/NullPointerException", "Firebase library name/version");
        return (klj_val){0};
    }
    // This registration only contributes SDK versions to telemetry headers.
    // With no backend, record it in the local boot log; publish nothing.
    KLJ_LOG("Firebase library %s %s (backend unavailable)", library, version);
    return (klj_val){0};
}
static klj_val shutdown_unstarted(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // RegisterLibrariesHelper immediately releases its cached classes. Neither
    // the Java log worker nor Google availability callbacks were started here.
    return (klj_val){0};
}
static klj_val backend_unavailable(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // AppOptions::LoadDefault and google_play_services::{CheckAvailability,
    // MakeAvailable} check/clear this exception and report failure. In
    // particular, MakeAvailable completes its future rather than waiting for
    // Android UI or callbacks that cannot run on this platform.
    klj_raise("java/lang/UnsupportedOperationException",
              "Firebase Android / Google Play services backend is unavailable on this host");
    return (klj_val){0};
}
static klj_val app_not_initialized(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    klj_raise("java/lang/IllegalStateException", "FirebaseApp has not been initialized");
    return (klj_val){0};
}
static klj_val gamestoty_unavailable(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // This SDK is an optional analytics/attribution bridge. Android would
    // initialize its Java backend here; visionOS has no matching service.
    KLJ_LOG_ONCE("Gamestoty analytics backend unavailable — initialization skipped");
    return (klj_val){0};
}

// Exact methods from the observed version-registration path and the SDK's
// default-app/dependency failure paths. Other Firebase calls remain unhandled.
static const klj_binding bindings[] = {
    {FB_REG, "getInstance", "()L" FB_REG ";", registrar},
    {FB_REG, "registerVersion", "(Ljava/lang/String;Ljava/lang/String;)V", register_version},
    {FB_LOG, "shutdown", "()V", shutdown_unstarted},
    {FB_GOOGLE_HELPER, "stopCallbacks", "()V", shutdown_unstarted},
    {FB_OPTIONS, "fromResource", "(Landroid/content/Context;)L" FB_OPTIONS ";", backend_unavailable},
    {GOOGLE_API, "getInstance", "()L" GOOGLE_API ";", backend_unavailable},
    {FB_APP, "getInstance", "()L" FB_APP ";", app_not_initialized},
    {FB_APP, "getInstance", "(Ljava/lang/String;)L" FB_APP ";", app_not_initialized},
    {NULL, NULL, NULL, NULL}
};
// Shave & Stuff's own analytics bridge, answered for that title alone.
static const klj_binding gamestoty_bindings[] = {
    {GAMESTOTY, "Init", "(Ljava/lang/String;Ljava/lang/String;)V", gamestoty_unavailable},
    {GAMESTOTY, "SendEvent", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
     gamestoty_unavailable},
    {NULL, NULL, NULL, NULL}
};

const klj_binding *klj_firebase_binding(const char *cls, const char *name, const char *sig) {
    const char *target = kl_driver_target_name();
    if (!target) return NULL;
    int shave = strcmp(target, "x7532973430077887") == 0;
    if (!shave && strcmp(target, "x9386827918099701") != 0) return NULL;
    for (const klj_binding *b = bindings; b->cls; b++)
        if (!strcmp(cls, b->cls) && !strcmp(name, b->name) && !strcmp(sig, b->sig)) return b;
    for (const klj_binding *b = gamestoty_bindings; shave && b->cls; b++)
        if (!strcmp(cls, b->cls) && !strcmp(name, b->name) && !strcmp(sig, b->sig)) return b;
    return NULL;
}
