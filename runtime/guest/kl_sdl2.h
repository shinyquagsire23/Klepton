// kl_sdl2 — the SDL2 SDLActivity front door.
//
// Counter-Strike VR (Xash3D) and the Source-engine ports (Half-Life 2 / Portal)
// are SDL2 apps: org.libsdl.app.SDLActivity loads libSDL2, and its onCreate runs
// the game's entry on a background thread through SDL's nativeRunMain(lib, fn).
// This is the SDL3 door kl_slink already implements, one major version down and
// with the entry library/function taken from the target rather than pinned:
//   - cs1     libxash.so     / SDL_main
//   - hl2/portal liblauncher.so / LauncherMainAndroid  (Source's SDL main)
// The library GRAPH is mapped by the shared kl_native loader; this file adds the
// SDL2-specific JNI handshake (JNI_OnLoad, the three nativeSetupJNI calls, the
// surface/resolution callbacks, and the SDL thread that calls nativeRunMain).
#ifndef KL_SDL2_H
#define KL_SDL2_H

#include <stdio.h>

int         kl_sdl2_configure(const char *libdir, const char *entry_lib, FILE *out);
const char *kl_sdl2_error(void);
int         kl_sdl2_load(FILE *out);     // map the graph (kl_native) + libSDL2 JNI_OnLoad
unsigned    kl_sdl2_gap(FILE *out);      // unresolved imports across the graph
int         kl_sdl2_begin(FILE *out);    // setupJNI + surface + spawn the SDL main thread
double      kl_sdl2_pump(double seconds, const volatile int *quit);
// Ask the guest to quit the way Android does when the activity is destroyed:
// SDLActivity.nativeSendQuit posts SDL_QUIT, the guest's own loop takes it and
// shuts down in order — which is where an engine writes its configuration.
// Returns 1 if the request was delivered.
int         kl_sdl2_send_quit(void);
int         kl_sdl2_main_returned(void);   // the guest's main is through
void        kl_sdl2_report(FILE *out);

#endif
