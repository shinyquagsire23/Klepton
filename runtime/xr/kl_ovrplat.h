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
// Reporting that the platform is unavailable is true here; answering "yes,
// entitled" to content we cannot see would not be.
//
// Walkabout courses are the narrow exception that stays on that side of the
// line. A course is reported installed, and its own store ids are reported
// purchased, only when the real track bundle is already on disk — files the
// user copied from a Quest they own. A stub bundle does not count. Download,
// delete, consume and checkout still abort.
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

// The completion queue and the call-surface table, exercised in-process: more
// requests outstanding than a pool block holds, popped handles held across new
// requests, ovr_FreeMessage misuse, details riding in the message, and both
// structures under concurrent threads. Returns 1 when everything held. Leaves
// the queue empty, but its requests and names stay counted — call it in a
// process of its own (tests/t_ovrplat.c), never in a guest run.
int kl_ovrplat_queue_selftest(FILE *f);

// Whether the installation gave this title a list of its asset files
// (KL_ASSET_FILES_PATH, or klepton-asset-files.txt in its files directory; see
// kl_assetcfg.h). Where it did, the asset-file questions are answered from it
// and the guest is told the device's language by default (kl_jni_services.c).
// Where it did not, nothing changes for any title.
int kl_ovrplat_asset_files_configured(void);

// KL_PLAT_TRACE=1: whether the asset-file, language-pack and locale answers
// are traced (default off). See kl_ovrplat.c.
int kl_ovrplat_trace_on(void);

#endif
