// libsteam_api.so for an Android guest — our stand-in for the Steamworks API.
//
// Steam ships Android builds of some VR games (for Steam Frame). Such a build
// carries Valve's own libsteam_api.so, and that library is a *client* of the
// Steam running on the headset: it dlopens libsteamclient.so and talks to it.
// There is no Steam on a Vision Pro, so the real library fails its own init and
// a game that insists on Steam quits ("Could not connect to Steam. Quitting...",
// Walkabout Mini Golf from depot 1408232).
//
// So the library is REPLACED, like libovrplatformloader: guest dlopen hands out
// a synthetic handle and dlsym answers by name. Unity games reach it through
// Steamworks.NET, which P/Invokes the flat API one function at a time, so every
// call the game makes arrives here with its name.
//
// What is answered, and on whose word. The same line as kl_ovrplat.h:
//   - Steam is "there", for one offline user. No friends, no servers, no cloud,
//     no auth tickets — an asynchronous request gets the invalid call handle.
//   - Ownership of add-ons (ISteamApps::BIsDlcInstalled & co.) is answered from
//     klepton-entitlements.txt and from nothing else: the list the Mac app wrote
//     from what Steam itself confirmed for the user's account. No file, no DLC.
//   - Every other function names itself once and answers 0.
#ifndef KL_STEAMAPI_H
#define KL_STEAMAPI_H
#include <stdio.h>

void *kl_steamapi_dlopen(const char *soname);   // NULL if not the Steam API
int   kl_steamapi_claims(const char *soname);   // the same test, without opening
int   kl_steamapi_is_handle(const void *h);
void *kl_steamapi_sym(const char *name);
// A Steam name a guest library IMPORTS (Unreal links the library and uses the
// SDK's C++ interfaces); NULL for a name that is not served.
void *kl_steamapi_lookup(const char *name);
// Has the guest opened the Steam API — is this the Steam build of its game?
int   kl_steamapi_in_use(void);
// The same question asked of the files, so it has an answer before the guest runs.
int   kl_steamapi_shipped(void);

// Which Steam functions the guest called and how they were answered.
void kl_steamapi_report(FILE *f);

#endif
