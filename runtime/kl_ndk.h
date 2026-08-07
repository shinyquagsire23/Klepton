// Android NDK surface (M3). The base set is what libunity.so imports:
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

// The surface Unity renders into. Placeholder until M5 binds a real drawable;
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

#endif
