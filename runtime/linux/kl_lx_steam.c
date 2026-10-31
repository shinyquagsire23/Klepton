// libsteam_api.so, offline: Steam as a single-player game needs it to be.
//
// Alyx links the Steamworks API and will not start without it — not for DRM, but
// because Source 2 routes even the loopback connection between its own client
// and server through ISteamNetworkingSockets, and reads the app id, the player's
// name and the language from the other interfaces on the way. There is no Steam
// client here to provide any of that, and on a headset there never will be one.
// So this stands in for the whole library, as a host library (kl_lx_hostlib.c):
// the two dozen flat functions the game imports, and behind them the interface
// objects those functions hand out.
//
// A Steamworks interface is a C++ object with nothing but virtual functions, so
// to the guest it is a pointer to a pointer to a table of code addresses, called
// with the object in x0 — which plain C functions here satisfy exactly. Every
// interface starts life as a table of 256 slots that each say which slot they
// are when first called and answer zero; the slots a game actually depends on
// are then overridden with real answers. The log of the rest is the work list,
// and it is why this file could be written against a game rather than against
// the whole SDK.
//
// What is answered is fixed and honest about being offline: one user, logged on,
// who owns the app; no overlay, no friends, no cloud, no servers.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kl_lx.h"
#include "kl_lx_steam_priv.h"
#include "../kl_env.h"

#define LX_STEAM_IFACES  64

static lx_iface *g_iface[LX_STEAM_IFACES];
static int g_niface;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// The default for every slot: name it once, answer zero. Zero is "false", "no
// handle", "k_EResultNone", NULL and an empty count all at once, which is the
// offline answer to most of the API.
static uint64_t slot_default(lx_iface *self, unsigned n) {
    if (!self->seen[n]) {
        self->seen[n] = 1;
        STEAM_LOG("%s: vtable slot %u called and not implemented — answering 0\n", self->name, n);
    }
    return 0;
}
#define SLOT(n) static uint64_t slot_##n(lx_iface *self) { return slot_default(self, n); }
#define ROW(h) SLOT(0x##h##0) SLOT(0x##h##1) SLOT(0x##h##2) SLOT(0x##h##3) SLOT(0x##h##4) \
               SLOT(0x##h##5) SLOT(0x##h##6) SLOT(0x##h##7) SLOT(0x##h##8) SLOT(0x##h##9) \
               SLOT(0x##h##a) SLOT(0x##h##b) SLOT(0x##h##c) SLOT(0x##h##d) SLOT(0x##h##e) \
               SLOT(0x##h##f)
ROW(0) ROW(1) ROW(2) ROW(3) ROW(4) ROW(5) ROW(6) ROW(7)
ROW(8) ROW(9) ROW(a) ROW(b) ROW(c) ROW(d) ROW(e) ROW(f)
#undef SLOT
#define SLOT(n) (void *)slot_##n,
static void *const k_default_table[LX_STEAM_SLOTS] = {
    ROW(0) ROW(1) ROW(2) ROW(3) ROW(4) ROW(5) ROW(6) ROW(7)
    ROW(8) ROW(9) ROW(a) ROW(b) ROW(c) ROW(d) ROW(e) ROW(f)
};
#undef SLOT
#undef ROW

// ---- the answers that are not zero ----
// Narrow arguments are taken as 32-bit and masked: the guest's ABI leaves the
// upper bits of a bool or a uint16 in a register unspecified, Darwin's assumes
// the caller cleaned them.
static uint64_t ret_true(lx_iface *s)  { (void)s; return 1; }
static uint64_t ret_one(lx_iface *s)   { (void)s; return 1; }

static uint32_t utils_GetAppID(lx_iface *s)             { (void)s; return LX_STEAM_APPID; }
static uint32_t utils_GetConnectedUniverse(lx_iface *s) { (void)s; return 1; }          // k_EUniversePublic
static uint32_t utils_GetServerRealTime(lx_iface *s)    { (void)s; return (uint32_t)time(NULL); }
static const char *utils_GetIPCountry(lx_iface *s)      { (void)s; return "US"; }
static const char *ret_english(lx_iface *s)             { (void)s; return "english"; }

static uint64_t user_GetSteamID(lx_iface *s)            { (void)s; return LX_STEAM_ID; }
static const char *friends_GetPersonaName(lx_iface *s)  { (void)s; return kl_env_str("KL_LX_STEAM_NAME", "Player"); }

static void set(lx_iface *i, unsigned slot, void *fn) { i->table[slot] = fn; }

// Slot numbers are the declaration order of the virtuals in the Steamworks SDK
// headers for the version named — the vtable IS that order.
static void specialise(lx_iface *i) {
    if (!strncmp(i->name, "SteamUtils", 10)) {
        set(i, 2, utils_GetConnectedUniverse);
        set(i, 3, utils_GetServerRealTime);
        set(i, 4, utils_GetIPCountry);
        set(i, 8, utils_GetAppID);
        set(i, 22, ret_english);                    // GetSteamUILanguage
    } else if (!strncmp(i->name, "SteamUser", 9) && strncmp(i->name, "SteamUserStats", 14)) {
        set(i, 0, ret_one);                         // GetHSteamUser
        set(i, 1, ret_true);                        // BLoggedOn
        set(i, 2, user_GetSteamID);
    } else if (!strncmp(i->name, "STEAMAPPS", 9)) {
        set(i, 0, ret_true);                        // BIsSubscribed
        set(i, 4, ret_english);                     // GetCurrentGameLanguage
        set(i, 5, ret_english);                     // GetAvailableGameLanguages
        set(i, 6, ret_true);                        // BIsSubscribedApp
    } else if (!strncmp(i->name, "SteamFriends", 12)) {
        set(i, 0, friends_GetPersonaName);
    } else if (!strncmp(i->name, "SteamNetworkingSockets", 22)) {
        kl_lx_steam_net_sockets(i);
    } else if (!strncmp(i->name, "SteamNetworkingUtils", 20)) {
        kl_lx_steam_net_utils(i);
    }
}

static void *iface(const char *version) {
    if (!version) return NULL;
    pthread_mutex_lock(&g_lock);
    lx_iface *i = NULL;
    for (int k = 0; k < g_niface && !i; k++)
        if (!strcmp(g_iface[k]->name, version)) i = g_iface[k];
    if (!i && g_niface < LX_STEAM_IFACES) {
        i = calloc(1, sizeof *i);
        snprintf(i->name, sizeof i->name, "%s", version);
        memcpy(i->table, k_default_table, sizeof i->table);
        i->vtbl = i->table;
        specialise(i);
        g_iface[g_niface++] = i;
        STEAM_LOG("interface %s handed out\n", version);
    }
    pthread_mutex_unlock(&g_lock);
    return i;
}

// ---- the flat API ----
static int32_t api_GetHSteamUser(void) { return 1; }
static int32_t api_GetHSteamPipe(void) { return 1; }
static int32_t api_zero(void)          { return 0; }
static void    api_nothing(void)       {}

static void *api_FindOrCreateUserInterface(int32_t user, const char *version) {
    (void)user;
    return iface(version);
}
static void *api_CreateInterface(const char *version) { return iface(version); }

// SteamInternal_SteamAPI_Init(versions, errmsg) -> ESteamAPIInitResult, 0 = OK.
static int32_t api_Init(const char *versions, char *err) {
    (void)versions;
    if (err) err[0] = 0;
    STEAM_LOG("SteamAPI_Init: offline, app %u, user '%s'\n", LX_STEAM_APPID,
              friends_GetPersonaName(NULL));
    return 0;
}

// The lazy accessor behind SteamUser(), SteamUtils() and the rest:
//   struct { void (*fn)(void *ctx); uintptr_t counter; <ctx> }
// fn is the GUEST's, and fills ctx by calling back into the two functions above.
// The counter is how the SDK re-initialises after a restart of the API; with one
// API lifetime it only has to stop being zero.
static void *api_ContextInit(void *data) {
    struct { void (*fn)(void *); uintptr_t counter; } *d = data;
    void *ctx = d + 1;
    if (d->counter != 1) {
        d->fn(ctx);
        d->counter = 1;
    }
    return ctx;
}

// Callbacks: registered and never fired. CCallbackBase is { vtable, flags, id };
// the flag is what the SDK's own destructor checks before unregistering.
static void api_RegisterCallback(void *cb, int32_t id) {
    struct { void *vt; uint8_t flags; int32_t id; } *c = cb;
    c->flags |= 1;
    c->id = id;
}
static void api_UnregisterCallback(void *cb) {
    struct { void *vt; uint8_t flags; int32_t id; } *c = cb;
    c->flags &= (uint8_t)~1;
}

static const kl_lx_hostsym k_syms[] = {
    { "SteamInternal_SteamAPI_Init",                  api_Init },
    { "SteamAPI_Init",                                api_zero },
    { "SteamAPI_InitAnonymousUser",                   ret_true },
    { "SteamAPI_Shutdown",                            api_nothing },
    { "SteamAPI_RunCallbacks",                        api_nothing },
    { "SteamAPI_GetHSteamUser",                       api_GetHSteamUser },
    { "SteamAPI_GetHSteamPipe",                       api_GetHSteamPipe },
    { "SteamInternal_ContextInit",                    api_ContextInit },
    { "SteamInternal_FindOrCreateUserInterface",      api_FindOrCreateUserInterface },
    { "SteamInternal_CreateInterface",                api_CreateInterface },
    { "SteamAPI_RegisterCallback",                    api_RegisterCallback },
    { "SteamAPI_UnregisterCallback",                  api_UnregisterCallback },
    { "SteamAPI_RegisterCallResult",                  api_nothing },
    { "SteamAPI_UnregisterCallResult",                api_nothing },
    { "SteamAPI_SetTryCatchCallbacks",                api_nothing },
    { "SteamAPI_UseBreakpadCrashHandler",             api_nothing },
    { "SteamAPI_SetBreakpadAppID",                    api_nothing },
    { "SteamAPI_SetMiniDumpComment",                  api_nothing },
    // No game server: a listen server inside the client does not register one.
    { "SteamInternal_GameServer_Init_V2",             api_zero },
    { "SteamInternal_FindOrCreateGameServerInterface", api_zero },
    { "SteamGameServer_GetHSteamUser",                api_zero },
    { "SteamGameServer_RunCallbacks",                 api_nothing },
    { "SteamGameServer_Shutdown",                     api_nothing },
};

static size_t build(kl_lx_hostsym **out) {
    size_t n = sizeof k_syms / sizeof *k_syms;
    *out = malloc(sizeof k_syms);
    memcpy(*out, k_syms, sizeof k_syms);
    return n;
}

void kl_lx_steam_init(void) { kl_lx_hostlib_register("libsteam_api.so", build); }
