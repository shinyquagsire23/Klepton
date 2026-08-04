// Android NDK surface (M3). Exactly the 27 symbols libunity.so actually imports:
// ALooper_* (6), ANativeWindow_* (6), ASensor* (15).
//
// Measured, not assumed — see tests/t_load.c output. Notably *absent* from the
// import list, and therefore not implemented here: AAssetManager_*,
// AConfiguration_*, and the whole ANativeActivity lifecycle. This APK is a
// com.unity3d.player.UnityPlayerActivity, so assets and configuration arrive
// over JNI and there is no ANativeActivity_onCreate anywhere in the chain.
#ifndef KL_NDK_H
#define KL_NDK_H
#include <stdint.h>

// Resolve an NDK import by name. NULL if we do not provide it.
void *kl_ndk_lookup(const char *name);

// The surface Unity renders into. Placeholder until M5 binds a real drawable;
// the host sets the true geometry once it has one.
void kl_ndk_set_window(int32_t width, int32_t height, int32_t format);
void *kl_ndk_window(void);

#endif
