// The Oculus Platform SDK front door — our stand-in for libovrplatformloader.so.
//
// Reconnaissance first, because it changes what this file has to be. The shipped
// libovrplatformloader.so is a *loader*, not an implementation: 1335 exports, and
// its only NEEDs are libc/libdl/liblog. Its strings name what it really does —
// bind to com.oculus.horizon / com.oculus.platformsdkruntime through
// com.oculus.platform.loader.EntryPoint. The actual platform lives in the Oculus
// system app on a real headset and is not in the APK at all.
//
// So on this machine the platform is *genuinely absent*, and no amount of loading
// the real library changes that: it runs, its async init can never complete, and
// then its own assert fires —
//   "ovr_PopMessage was called before ovr_PlatformInitializeAndroid()!"
// — which is what killed the run right after the first presented frame.
//
// We therefore serve the soname ourselves, same shape as the GL, OpenSL and
// OVRPlugin gateways.
//
// ---------------------------------------------------------------------------
// The DRM line, which is the whole point of this file existing separately
//
// Of those 1335 exports, exactly two concern entitlement. The rest is social and
// services plumbing — Message, Room, User, Voip, Party, NetSync, Matchmaking,
// Leaderboard, CloudStorage, Achievements — none of which can work here anyway,
// because there is no backing runtime to forward to.
//
// There is also no cryptographic DRM anywhere in this: the loader links no crypto,
// and the game's own assets are plaintext (IL2CPP metadata reads with strings(1);
// 71 files under assets/bin/Data hold plaintext GLSL ES). Entitlement here is a
// policy answer from a network service, not an encryption key.
//
// That makes it *easy* to fake, which is exactly why this file refuses to.
// Ownership and purchase queries do not get a stub, do not get a permissive zero,
// and are not reachable by accident: kl_ovrplat_sym routes them to a handler that
// always aborts naming the entry point, whatever KL_PERMISSIVE says. Everything
// else is measured and answered as the trace forces it.
//
// PLANNING §8. Reporting that the platform is unavailable is true here; answering
// "yes, entitled" would not be.
#ifndef KL_OVRPLAT_H
#define KL_OVRPLAT_H
#include <stdio.h>

void *kl_ovrplat_dlopen(const char *soname);   // NULL if not the platform loader
int   kl_ovrplat_claims(const char *soname);   // the same test, without opening
int   kl_ovrplat_is_handle(const void *h);
void *kl_ovrplat_sym(const char *name);

// Which ovr_* the guest resolved and called, and which DRM entry points it asked
// for — the last of those is worth seeing even when nothing aborted.
void kl_ovrplat_report(FILE *f);

#endif
