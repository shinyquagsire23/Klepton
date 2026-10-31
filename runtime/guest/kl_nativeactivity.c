// Android's NativeActivity door. See kl_nativeactivity.h for why it is its own
// file rather than a copy in each of the two guests that take it.
//
// Lifted verbatim out of kl_slink.c when RE4 (Unreal Engine 4) became the
// second guest through it — the sequence, the diagnostics and the local frame
// discipline are Steam Link's, proven across the whole SL arc, and nothing here
// is new behaviour.
#include "kl_nativeactivity.h"
#include <sys/stat.h>
#include "klepton.h"

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

// What a game's own Java onCreate does after NativeActivity.onCreate, for the
// games whose Java hands the engine its paths through a native rather than the
// ANativeActivity struct. HITMAN 3 VR: Reloaded (x8012387035448196, XR Games'
// com.xrgames.MainActivity) calls nativeSetAndroidInfo(packageName,
// getObbDir(), getExternalFilesDir(null), Locale.getDefault()) there; the
// engine keeps them as ZG2Android::packageName/obbFilePath/dataFilePath/
// language. Never called, the package manager looked for
// "//packagedefinition.txt", found nothing and stopped on its own trap
// (ZPackageManagerBase, Mac run 2026-10-20). Only a library that exports the
// native is called, and it is called just BEFORE onCreate (see kl_na_create).
typedef void (*kl_na_set_android_info_fn)(void *env, void *thiz, void *pkg, void *obb,
                                          void *files, void *locale);
static void kl_na_java_on_create(kl_image *img, const char *data_path, FILE *out) {
    kl_na_set_android_info_fn set_info =
        (kl_na_set_android_info_fn)kl_sym(img, "Java_com_xrgames_MainActivity_nativeSetAndroidInfo");
    if (!set_info) return;
    const char *pkg = kl_jni_guest_package();
    char obb[256], locale[32], lang[16] = "en", country[16] = "US";
    // The guest's /sdcard is the files folder (kl_guest_path), and a store
    // game's extra files are installed under Android/obb/<package> there.
    snprintf(obb, sizeof obb, "/sdcard/Android/obb/%s", pkg ? pkg : "");
    kl_jni_locale_parts(lang, sizeof lang, country, sizeof country);
    snprintf(locale, sizeof locale, "%s_%s", lang, country);
    // getObbDir() and getExternalFilesDir() create their folders on Android; the
    // game takes them as there. Its save manager asks statvfs about the files
    // folder before every save and fails the save when that fails — "SAVE
    // FAILED" on the headset and on the Mac, whose /sdcard had no files/ yet
    // (2026-10-20). So both are made here, under their host names.
    const char *dirs[2] = { obb, data_path };
    for (int d = 0; d < 2; d++) {
        char hp[1024];
        const char *h = kl_guest_path(dirs[d], hp, sizeof hp);
        char part[1024];
        snprintf(part, sizeof part, "%s", h ? h : "");
        for (char *c = part + 1; *c; c++)
            if (*c == '/') { *c = 0; mkdir(part, 0755); *c = '/'; }
        if (*part) mkdir(part, 0755);
    }
    if (out) {
        fprintf(out, "  [na] nativeSetAndroidInfo(%s, %s, %s, %s) — the game's Java onCreate\n",
                pkg ? pkg : "(null)", obb, data_path, locale);
        fflush(out);
    }
    set_info(g_act.env, g_act.clazz, kl_jni_new_string(pkg ? pkg : ""), kl_jni_new_string(obb),
             kl_jni_new_string(data_path), kl_jni_new_string(locale));
}

int kl_na_create(kl_image *img, const char *entry, const char *data_path, FILE *out) {
    if (!img) {
        if (out) fprintf(out, "  [na] no image to enter\n");
        return 1;
    }
    if (!entry) entry = "ANativeActivity_onCreate";
    if (!data_path) data_path = kl_jni_files_dir();

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
    g_act.internalDataPath = data_path;
    g_act.externalDataPath = data_path;
    // 29, the same Quest-2 answer Build.SDK_INT gives. Two numbers describing
    // one device have to agree — this is the display-panel group answer again.
    g_act.sdkVersion       = 29;
    g_act.assetManager     = kl_ndk_asset_manager();
    // Short /sdcard alias, not the ~90-char container path from kl_jni_files_dir():
    // both resolve to the same place via kl_guest_path, but the guest reads obbPath
    // straight from the struct and a game that appends+FORTIFY-strcats into a fixed
    // buffer overflows on the long form (vicecity's 128-byte casepath buffer).
    g_act.obbPath          = "/sdcard";

    if (out) {
        fprintf(out, "  activity: clazz=%p env=%p assets=%p sdk=%d dataPath=%s\n",
                g_act.clazz, g_act.env, g_act.assetManager, g_act.sdkVersion,
                g_act.internalDataPath ? g_act.internalDataPath : "(null)");
        fflush(out);
    }

    kl_jni_local_frame_push();
    // Before onCreate, not after it as Java orders them: onCreate starts the
    // engine's own thread, and HITMAN's went straight on to its package
    // manager. When that thread won the race the paths were still empty — it
    // opened "//packagedefinition.txt" and stopped on its own trap (headset,
    // 2026-10-20, a run in German that happened to be quicker; the same build
    // in English had set them first). The native only copies the four strings
    // into the engine's buffers, so it needs nothing onCreate makes.
    kl_na_java_on_create(img, data_path, out);
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
