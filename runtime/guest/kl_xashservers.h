// A player's own game servers for a Xash3D guest: see kl_xashservers.c.
#ifndef KL_XASHSERVERS_H
#define KL_XASHSERVERS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>

// At engine set-up: read <base>/avpplay-servers.txt, add its servers to the game's favourites, write (or
// remove) the remote-admin copies of the menus, and add the placeholder rcon_password to KL_XASH_CVARS.
// `assets` is where the packaged files of the guest are readable; `out` is the boot log, or NULL.
void kl_xash_servers_prepare(const char *base, const char *gamedir, const char *assets, FILE *out);

// For the network layer: a datagram about to go to `to` (a HOST sockaddr). If it is the guest's rcon request
// with the placeholder password and `to` is a listed server with admin access, the datagram with that server's
// password is written to `out` and its length returned; otherwise 0, and the datagram goes out as it is.
size_t kl_xash_rcon_rewrite(const void *buf, size_t n, const struct sockaddr *to, char *out, size_t cap);

// For the network layer, after a datagram has gone to `to` (a HOST sockaddr): if it was the engine asking for
// servers (its request to a master server, or its "info" query), each listed server is queried too, from the
// same socket and in its own dialect, so that their answers put them into the game's server list.
void kl_xash_servers_on_query(int fd, const void *buf, size_t n, const struct sockaddr *to, socklen_t tolen);

// For name resolution, when `name` did not resolve: a stand-in address if it is one of the guest's master
// servers that no longer exist and the player has servers listed; otherwise NULL.
const char *kl_xash_servers_master_standin(const char *name);

#endif
