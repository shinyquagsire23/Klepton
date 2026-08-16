// Android's NativeActivity door — the OTHER way a guest is started.
//
// This project's first five guests are Unity, and a Unity guest is entered
// through `libmain`'s JNI_OnLoad and then DRIVEN by calling natives it
// registered (`UnityPlayer.nativeRender` and friends). A NativeActivity guest
// is the other shape: the framework hands it an `ANativeActivity` and it fills
// in a table of CALLBACKS, after which the lifecycle runs by calling those.
//
// Two targets take this door and they arrived two years apart:
//
//   * Steam Link's VR front door (`libvrlink_scene.so`), which is one library
//     and not an engine.
//   * **Unreal Engine 4** (`libUE4.so`), which is an engine and a whole second
//     dialect of "an Android game" — RE4 is the first (kl_ue4.c).
//
// It lives here rather than in either of them because the struct below is
// **ABI**, not an interface we get to design: the guest's glue reads these
// fields by offset out of the pointer we hand `ANativeActivity_onCreate`, and
// writes its callbacks back through the first one. Two copies of an ABI struct
// is the exact failure this tree keeps paying for — one gets a field added and
// the other silently reads a shifted offset, which is a wild pointer with no
// error surface anywhere.
#ifndef KL_NATIVEACTIVITY_H
#define KL_NATIVEACTIVITY_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include "klepton.h"

// Transcribed from <android/native_activity.h>.
//
// `clazz` is the NDK's own misnomer — it is the activity INSTANCE object, not a
// jclass, and the guest calls getIntent()/getPackageName() on it. Ours is the
// jobject kl_jni hands out for the activity, so those land on g_bindings and
// fail by name like every other gap in the surface.
typedef struct kl_ANativeActivity kl_ANativeActivity;
typedef struct {
    void (*onStart)(kl_ANativeActivity *);
    void (*onResume)(kl_ANativeActivity *);
    void *(*onSaveInstanceState)(kl_ANativeActivity *, size_t *);
    void (*onPause)(kl_ANativeActivity *);
    void (*onStop)(kl_ANativeActivity *);
    void (*onDestroy)(kl_ANativeActivity *);
    void (*onWindowFocusChanged)(kl_ANativeActivity *, int);
    void (*onNativeWindowCreated)(kl_ANativeActivity *, void *);
    void (*onNativeWindowResized)(kl_ANativeActivity *, void *);
    void (*onNativeWindowRedrawNeeded)(kl_ANativeActivity *, void *);
    void (*onNativeWindowDestroyed)(kl_ANativeActivity *, void *);
    void (*onInputQueueCreated)(kl_ANativeActivity *, void *);
    void (*onInputQueueDestroyed)(kl_ANativeActivity *, void *);
    void (*onContentRectChanged)(kl_ANativeActivity *, const void *);
    void (*onConfigurationChanged)(kl_ANativeActivity *);
    void (*onLowMemory)(kl_ANativeActivity *);
} kl_ANativeActivityCallbacks;
struct kl_ANativeActivity {
    kl_ANativeActivityCallbacks *callbacks;
    void       *vm;
    void       *env;
    void       *clazz;
    const char *internalDataPath;
    const char *externalDataPath;
    int32_t     sdkVersion;
    void       *instance;
    void       *assetManager;
    const char *obbPath;
};

// Run `ANativeActivity_onCreate` in `img`, having filled the activity in from
// the JNI and NDK surfaces. 0 on success; on failure it returns non-zero and
// has already said why on `out`.
//
// `entry` names the symbol so a guest that exports it under another name is a
// row rather than a fork; every caller so far passes
// "ANativeActivity_onCreate".
int kl_na_create(kl_image *img, const char *entry, FILE *out);

// The rest of what Android's NativeActivity does, in its order — onStart,
// onResume, the window, focus. onCreate itself typically only spawns the
// guest's thread and returns; nothing renders until the window arrives, and the
// glue's own loop blocks until it does.
void kl_na_start(FILE *out);

// ...and the way back out, for a driver that wants a clean shutdown rather than
// an exit: focus, window destroyed, pause, stop, destroy.
void kl_na_stop(FILE *out);

// How many of the sixteen callbacks onCreate actually installed. The cheapest
// confirmation that it did its job — a glue that returned early leaves the
// table empty, and that reads identically to "it worked" without asking.
int kl_na_callbacks_registered(void);

// The activity we handed the guest, for a driver that needs to pass it back
// (an input queue, a configuration change).
kl_ANativeActivity *kl_na_activity(void);

// One lifecycle hook, by name, for the callbacks that are not part of the
// standard start/stop pair — the input queue especially, which UE4 needs and
// Steam Link never used.
const kl_ANativeActivityCallbacks *kl_na_callbacks(void);

#endif
