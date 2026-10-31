// What kl_lx_steam.c and kl_lx_steam_net.c share: the shape of an interface
// object. See kl_lx_steam.c for what one is.
#ifndef KL_LX_STEAM_PRIV_H
#define KL_LX_STEAM_PRIV_H
#include <stdint.h>
#include <stdio.h>

#define LX_STEAM_APPID   546560u                 /* Half-Life: Alyx */
#define LX_STEAM_ID      0x0110000100000001ull   /* universe public, individual, account 1 */
#define LX_STEAM_SLOTS   256

typedef struct lx_iface {
    void **vtbl;                    // what the guest dereferences: must be first
    char   name[64];
    void  *table[LX_STEAM_SLOTS];
    uint8_t seen[LX_STEAM_SLOTS];
} lx_iface;

#define STEAM_LOG(...) fprintf(stderr, "  [steam] " __VA_ARGS__)

// ISteamNetworkingSockets and ISteamNetworkingUtils, in-process (kl_lx_steam_net.c).
void kl_lx_steam_net_sockets(lx_iface *i);
void kl_lx_steam_net_utils(lx_iface *i);

#endif
