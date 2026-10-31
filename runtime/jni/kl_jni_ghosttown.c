// Ghost Town (x9531494193591052) builds its Quest platform and then constructs
// its own Java class DetectHeadphones. The class is not Android and not a
// shared SDK: it lives in the default package of this one APK. With no row
// for it, the runtime stops at the constructor
// ("no host implementation for DetectHeadphones.<init>…") and the process
// ends during PlatformManager startup, before a frame is shown.
//
// The class, as this build declares it:
//   <init>(Context)                         stores the activity
//   StartDetecting(String gameObject)       registers a BroadcastReceiver for
//                                           ACTION_HEADSET_PLUG
//   StopDetecting()                         unregisters that receiver
// The receiver's only job is UnityPlayer.UnitySendMessage(gameObject,
// "_HeadphonesPluggedIn" / "_HeadphonesUnplugged", "") when the plug state
// changes. It does not read the sticky Intent registerReceiver returns, so
// on a Quest the game also hears nothing until a later plug or unplug.
//
// There is no headset jack here and nothing publishes ACTION_HEADSET_PLUG,
// so StartDetecting records the subscription and delivers no event — the
// same answer Context.registerReceiver already gives for a filter that is
// not the sticky battery query. StopDetecting tears that subscription down.
// For this title only: any other guest that asked for the same class name
// still stops, so the row cannot change a game it was not written for.
#include <string.h>
#include "guest/kl_driver.h"
#include "kl_jni_int.h"

#define GH_TARGET "x9531494193591052"
#define GH_CLASS  "DetectHeadphones"

static int gh_this_title(void) {
    const char *t = kl_driver_target_name();
    return t && strcmp(t, GH_TARGET) == 0;
}

// One reference argument and a void return. Unity spells the constructor
// from the runtime type of the activity it holds, which is
// (Lcom.unity3d.player.UnityPlayerActivity;)V — dots, not the slashes the
// class file uses for Context. The argument is not kept: the receiver it
// would be registered on never fires here.
static int gh_init_sig(const char *sig) {
    if (!sig || sig[0] != '(' || sig[1] != 'L') return 0;
    const char *semi = strchr(sig, ';');
    return semi && semi[1] == ')' && semi[2] == 'V' && semi[3] == 0;
}

static int gh_sig_eq(const char *sig, const char *want) {
    if (!sig || !want) return 0;
    for (; *sig && *want; sig++, want++) {
        char a = *sig == '.' ? '/' : *sig;
        char b = *want == '.' ? '/' : *want;
        if (a != b) return 0;
    }
    return *sig == *want;
}

static klj_val gh_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("DetectHeadphones.<init>(Context) — headphone jack is not present "
            "on this host; plug events will not arrive");
    return (klj_val){.l = kl_jni_new_object(GH_CLASS)};
}

static klj_val gh_start(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *go = n > 0 ? klj_str(a[0].l) : NULL;
    KLJ_LOG("DetectHeadphones.StartDetecting(\"%s\") — ACTION_HEADSET_PLUG "
            "has no publisher here, so no _HeadphonesPluggedIn / "
            "_HeadphonesUnplugged message is sent",
            go ? go : "");
    return (klj_val){0};
}

static klj_val gh_stop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("DetectHeadphones.StopDetecting() — nothing was registered");
    return (klj_val){0};
}

static const klj_binding k_init = {GH_CLASS, "<init>", "(Landroid/content/Context;)V", gh_init};
static const klj_binding k_start = {GH_CLASS, "StartDetecting", "(Ljava/lang/String;)V", gh_start};
static const klj_binding k_stop = {GH_CLASS, "StopDetecting", "()V", gh_stop};

const klj_binding *klj_ghosttown_binding(const char *cls, const char *name, const char *sig) {
    if (!gh_this_title() || !cls || strcmp(cls, GH_CLASS) != 0 || !name) return NULL;
    if (strcmp(name, "<init>") == 0 && gh_init_sig(sig)) return &k_init;
    if (strcmp(name, "StartDetecting") == 0 && gh_sig_eq(sig, k_start.sig)) return &k_start;
    if (strcmp(name, "StopDetecting") == 0 && sig && strcmp(sig, "()V") == 0) return &k_stop;
    return NULL;
}
