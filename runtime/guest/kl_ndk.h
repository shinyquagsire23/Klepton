// Android NDK surface. The base set is what libunity.so imports:
// ALooper_*, ANativeWindow_*, ASensor*. The Steam Link target (SDL3) adds
// ANativeWindow_lock/unlockAndPost and the AAssetManager_*/AAsset_* subset —
// Unity never imported those (its assets arrive over JNI), which is why they
// were absent from the original measured surface rather than refused.
// Still absent: AConfiguration_* and the whole ANativeActivity lifecycle —
// neither target has an ANativeActivity_onCreate anywhere in the chain.
#ifndef KL_NDK_H
#define KL_NDK_H
#include <stdint.h>

// Resolve an NDK import by name. NULL if we do not provide it.
void *kl_ndk_lookup(const char *name);

// The surface Unity renders into. Placeholder until EGL binds a real drawable;
// the host sets the true geometry once it has one.
void kl_ndk_set_window(int32_t width, int32_t height, int32_t format);
void *kl_ndk_window(void);
// Geometry of an ANativeWindow the guest handed us. kl_egl.c sizes its surfaces
// from this rather than keeping its own copy: Unity derives the render target
// from whatever eglQuerySurface reports, and a second set of numbers that
// disagreed with the window would be worse than either alone.
void kl_ndk_window_size(const void *win, int32_t *w, int32_t *h);

// Root directory for AAssetManager_open() — the unpacked APK's assets dir.
void kl_ndk_set_assets_dir(const char *dir);

// A window that is NOT the activity's. AImageReader_getWindow hands the codec
// one of these as its output surface, and the guest may then call any
// ANativeWindow_* entry point on it — so it has to be a real window of the same
// kind rather than a token, or getWidth on the decoder's surface answers with
// the activity's size.
//
// `owner` is opaque here on purpose: kl_ndk has no business knowing what an
// AImageReader is, and the codec needs to get from the window it was configured
// with back to the reader that made it. Whoever creates the window says what it
// belongs to; kl_ndk_window_owner hands that back unexamined.
void *kl_ndk_window_new(int32_t w, int32_t h, int32_t format, void *owner);
void  kl_ndk_window_free(void *win);
void *kl_ndk_window_owner(const void *win);   // NULL for the activity's window

// A producer surface's size is the PRODUCER's, and it is not known until the
// first frame arrives — AImageReader is created 1x1 and corrected here.
void kl_ndk_window_set_size(void *win, int32_t w, int32_t h);

// The AAssetManager* an ANativeActivity carries. Same object
// AAssetManager_fromJava hands out — see the note there.
void *kl_ndk_asset_manager(void);

// Give the CALLING thread a looper, as Android's main thread always has one
// before any activity runs. A NativeActivity host must do this before
// ANativeActivity_onCreate — see the note at the definition for what it costs
// not to.
void kl_ndk_prepare_looper(void);

// Does the CALLING thread have one? The native half of Java's
// Looper.myLooper(), which is the same question asked from the other side of
// the JNI boundary — and the two must agree, or a guest that checks through
// Java and then acts through ALooper gets contradictory answers.
int kl_ndk_thread_has_looper(void);

// ...and pump it, which is what Looper.loop() does on Android's main thread
// between callbacks. Returns what ALooper_pollOnce returns. A prepared looper
// nobody polls is a queue with no drain — see the note at the definition.
int kl_ndk_pump_looper(int timeout_ms);

#endif
