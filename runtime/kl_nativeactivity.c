// Android's NativeActivity door. See kl_nativeactivity.h for why it is its own
// file rather than a copy in each of the two guests that take it.
//
// Lifted verbatim out of kl_slink.c when RE4 (Unreal Engine 4) became the
// second guest through it — the sequence, the diagnostics and the local frame
// discipline are Steam Link's, proven across the whole SL arc, and nothing here
// is new behaviour.
#include "kl_nativeactivity.h"

#include "kl_jni.h"
#include "kl_ndk.h"

static kl_ANativeActivityCallbacks g_cbs;
static kl_ANativeActivity g_act;

typedef void (*anativeactivity_oncreate_fn)(kl_ANativeActivity *, void *, size_t);

// One call per lifecycle hook, named, so a NULL callback is distinguishable
// from one that ran. Android calls these from the UI thread; so do we.
#define NA_CB(out, name, ...)                                                  \
    do {                                                                       \
        if (g_cbs.name) {                                                      \
            if (out) { fprintf(out, "  [na] %s\n", #name); fflush(out); }       \
            kl_jni_local_frame_push();                                          \
            g_cbs.name(&g_act, ##__VA_ARGS__);                                  \
            kl_jni_local_frame_pop();                                           \
        } else if (out) fprintf(out, "  [na] %s — not registered\n", #name);    \
    } while (0)

int kl_na_create(kl_image *img, const char *entry, FILE *out) {
    if (!img) {
        if (out) fprintf(out, "  [na] no image to enter\n");
        return 1;
    }
    if (!entry) entry = "ANativeActivity_onCreate";

    anativeactivity_oncreate_fn onCreate =
        (anativeactivity_oncreate_fn)kl_sym(img, entry);
    if (!onCreate) {
        if (out) fprintf(out, "  [na] the image exports no %s\n", entry);
        return 1;
    }

    // This thread is the app's UI thread, and on Android that means it has a
    // looper before any activity is created. The guest takes it with
    // ALooper_forThread() inside onCreate and does not check for NULL.
    kl_ndk_prepare_looper();

    g_act.callbacks        = &g_cbs;
    g_act.vm               = kl_jni_vm();
    g_act.env              = kl_jni_env();
    g_act.clazz            = kl_jni_activity();
    g_act.internalDataPath = kl_jni_files_dir();
    g_act.externalDataPath = kl_jni_files_dir();
    // 29, the same Quest-2 answer Build.SDK_INT gives. Two numbers describing
    // one device have to agree — this is the display-panel group answer again.
    g_act.sdkVersion       = 29;
    g_act.assetManager     = kl_ndk_asset_manager();
    g_act.obbPath          = kl_jni_files_dir();

    if (out) {
        fprintf(out, "  activity: clazz=%p env=%p assets=%p sdk=%d dataPath=%s\n",
                g_act.clazz, g_act.env, g_act.assetManager, g_act.sdkVersion,
                g_act.internalDataPath ? g_act.internalDataPath : "(null)");
        fflush(out);
    }

    kl_jni_local_frame_push();
    onCreate(&g_act, NULL, 0);
    kl_jni_local_frame_pop();
    if (out) {
        fprintf(out, "  onCreate returned; %d of %zu callbacks registered\n",
                kl_na_callbacks_registered(), sizeof g_cbs / sizeof(void *));
        fflush(out);
    }
    return 0;
}

void kl_na_start(FILE *out) {
    NA_CB(out, onStart);
    NA_CB(out, onResume);
    NA_CB(out, onNativeWindowCreated, kl_ndk_window());
    NA_CB(out, onWindowFocusChanged, 1);
}

void kl_na_stop(FILE *out) {
    NA_CB(out, onWindowFocusChanged, 0);
    NA_CB(out, onNativeWindowDestroyed, kl_ndk_window());
    NA_CB(out, onPause);
    NA_CB(out, onStop);
    NA_CB(out, onDestroy);
}

int kl_na_callbacks_registered(void) {
    int n = 0;
    void **slot = (void **)&g_cbs;
    for (size_t i = 0; i < sizeof g_cbs / sizeof(void *); i++) n += slot[i] != NULL;
    return n;
}

kl_ANativeActivity *kl_na_activity(void) { return &g_act; }

const kl_ANativeActivityCallbacks *kl_na_callbacks(void) { return &g_cbs; }
