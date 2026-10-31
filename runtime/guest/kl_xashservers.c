// A player's own game servers for a Xash3D guest (Counter-Strike VR): the list,
// the admin access, and the menus that use it.
//
// What the guest offers on its own: a server browser with a "Favorites" tab read
// from <gamedir>/favorite_servers.lst, an `rcon` command that sends
// "rcon <rcon_password> <command>" to the server it is connected to, and a quick
// menu (left stick press, `exec touch/cmd/cmd`) whose "Server Settings" and bots
// pages run SERVER commands — `bot_add`, `changelevel de_dust2`, `mp_friendlyfire 1`.
// Those only do anything when the player is the host. On somebody's dedicated
// server they are forwarded as ordinary client commands and ignored, and in a
// headset there is no console to type `rcon_password` and `rcon …` into.
//
// So, from one small file the installer stages next to the game data
// (<base>/avpplay-servers.txt):
//
//     server  <host[:port]> [name]       a server to show in the game's list
//     admin   <host[:port]> <password>   its admin (rcon) password
//     goldsrc <host[:port]>              it is a classic Half-Life server, not a Xash3D one
//
//  1. In the list: Team Beef's menu has no Favorites tab any more (only "Servers"), so a
//     favourites entry is never shown. What fills that one list, read from the menu's source
//     and seen on the headset:
//       - its built-in servers: for each, the MENU opens a socket of its own, sends a Source
//         query, takes whatever datagram comes back within 300 ms as proof of life, and lists
//         the server under the name it has built in. Nothing can be added there, and a
//         datagram of ours on that socket would be mistaken for the probed server's answer;
//       - whatever answers the ENGINE: it asks the master servers ("1\xff0.0.0.0:0…") or, in
//         "LAN games", broadcasts "info 49"; every "info" answer that then arrives on the
//         engine's socket is listed, whoever it is from.
//     So when the engine sends either of those two (kl_xash_servers_on_query), each listed
//     server is asked "info 49" from the same socket, and its answer puts it into the list
//     under its real name. The menu's own probe is left alone.
//     One more thing is needed for "Internet Games": the engine's master servers
//     (mentality.rip and two more names under it) no longer resolve, and an engine that cannot
//     resolve a master never sends its request – which is why Team Beef built servers into the
//     menu. With servers listed, such a name that fails to resolve gets a stand-in address
//     from a range reserved for documentation (kl_xash_servers_master_standin): the request
//     goes out to nobody, and it is the cue for asking the player's servers.
//  2. Admin access by destination: the guest gets a placeholder rcon_password, and
//     an outgoing "rcon" datagram whose destination is a listed server has the
//     placeholder replaced by that server's password (kl_xash_rcon_rewrite, called
//     from the sendto shim). The guest never needs to know which server it joined,
//     a list of several servers with different passwords just works, and a server
//     that is NOT in the list never sees a real password.
//  3. The same menus, remotely: copies of the bots and server-settings pages are
//     written into the game folder (a loose file wins over the packaged one), with
//     each server command prefixed by `$avp_rc` — empty while the player hosts
//     (host_serverstate != 0), "rcon" on a remote server.
//     A value field (type 3) is `messagemode <cvar>`: the guest reads a line and runs
//     `<cvar> "<line>"`, and messagemode takes exactly ONE word, so `rcon <cvar>` cannot be
//     put there. On a remote server the field therefore becomes `messagemode rcon` with the
//     setting's name riding in the placeholder password (`avpplay+mp_timelimit`); the rewrite
//     in (2) puts it back in front of the value: "rcon <password> mp_timelimit 30". Every
//     page resets the placeholder when it is drawn, so the name never sticks to a later command.
//
// With no `admin` line the placeholder is not set and earlier copies of the menus
// are removed, so a player without servers sees exactly the game as it ships.
#include "kl_xashservers.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KLXS_MAX 32
#define KLXS_PLACEHOLDER "avpplay"
#define KLXS_MARK "// avpplay: remote admin menu, generated at start from the packaged one\n"

typedef struct {
    char addr[128];          // host:port as written (port added when missing)
    char password[128];      // empty: listed, but no admin access
    int  goldsrc;            // a classic Half-Life server: asked with the Source query
    int  resolved;           // 0 not tried, 1 ok, -1 failed
    double asked;            // when it was last sent a list query (monotonic seconds)
    uint32_t ip;             // network order
    uint16_t port;           // network order
} klxs_server;

static klxs_server g_srv[KLXS_MAX];
static int g_nsrv, g_nadmin;

static klxs_server *klxs_find(const char *addr) {
    for (int i = 0; i < g_nsrv; i++)
        if (!strcasecmp(g_srv[i].addr, addr)) return &g_srv[i];
    return NULL;
}

// "host" -> "host:27015"; anything that is not a plain host[:port] is refused.
static int klxs_normalize(const char *in, char *out, size_t cap) {
    size_t n = strlen(in);
    if (!n || n > 100) return -1;
    int colons = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == ':') colons++;
        else if (!(isalnum(c) || c == '.' || c == '-' || c == '_')) return -1;
    }
    if (colons > 1) return -1;
    snprintf(out, cap, colons ? "%s" : "%s:27015", in);
    return 0;
}

static void klxs_parse(const char *path, FILE *out) {
    g_nsrv = g_nadmin = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char kind[16] = "", addr[128] = "", rest[256] = "";
        if (sscanf(line, " %15s %127s %255[^\r\n]", kind, addr, rest) < 2 || kind[0] == '#') continue;
        char norm[128];
        if (klxs_normalize(addr, norm, sizeof norm)) continue;
        klxs_server *s = klxs_find(norm);
        if (!s) {
            if (g_nsrv >= KLXS_MAX) continue;
            s = &g_srv[g_nsrv++];
            memset(s, 0, sizeof *s);
            snprintf(s->addr, sizeof s->addr, "%s", norm);
        }
        if (!strcmp(kind, "goldsrc")) s->goldsrc = 1;
        if (!strcmp(kind, "admin")) {
            // An rcon password is one token on the wire; one with a space or a quote could not be sent.
            if (!rest[0] || strpbrk(rest, " \t\"") ) continue;
            snprintf(s->password, sizeof s->password, "%s", rest);
        }
    }
    fclose(f);
    for (int i = 0; i < g_nsrv; i++) if (g_srv[i].password[0]) g_nadmin++;
    if (out) fprintf(out, "  [xash] servers: %d listed, %d with admin access\n", g_nsrv, g_nadmin);
}

// ---------------------------------------------------------------------------
// 1. Favorites

static void klxs_favorites(const char *gamepath, FILE *out) {
    if (!g_nsrv) return;
    char path[1600];
    snprintf(path, sizeof path, "%s/favorite_servers.lst", gamepath);
    char have[8192] = "";
    FILE *f = fopen(path, "r");
    if (f) { size_t n = fread(have, 1, sizeof have - 1, f); have[n] = 0; fclose(f); }
    int added = 0;
    f = NULL;
    for (int i = 0; i < g_nsrv; i++) {
        if (strstr(have, g_srv[i].addr)) continue;
        if (!f && !(f = fopen(path, "a"))) return;
        if (!added && have[0] && have[strlen(have) - 1] != '\n') fputc('\n', f);
        fprintf(f, "%s %s\n", g_srv[i].addr, g_srv[i].goldsrc ? "gs" : "49");   // the menu's own format: address, protocol
        added++;
    }
    if (f) fclose(f);
    if (out && added) fprintf(out, "  [xash] servers: %d added to the game's favourites\n", added);
}

// ---------------------------------------------------------------------------
// 3. The menus

// The pages of the quick menu that run server commands, relative to touch/.
static const char *const k_menus[] = {
    "cmd/cmd.cfg", "bots/bots.cfg",
    "bots/my_menu-2-addbot.cfg", "bots/my_menu-2-settings.cfg", "bots/my_menu-3-quota.cfg",
    "bots/my_menu-3-set1.cfg", "bots/my_menu-3-set2.cfg", "bots/my_menu-3-set3.cfg",
    "bots/my_menu-3-set4.cfg", "bots/my_menu-3-set5.cfg", "bots/my_menu-3-set6.cfg",
    "cmd/my_menu-2-server.cfg", "cmd/my_menu-3-maps.cfg",
    "cmd/my_menu-4-cvar0.cfg", "cmd/my_menu-4-cvar1.cfg", "cmd/my_menu-4-cvar2.cfg",
    "cmd/my_menu-4-cvar3.cfg", "cmd/my_menu-4-cvar4.cfg", "cmd/my_menu-4-cvar5.cfg",
    "cmd/my_menu-5-cvar0-2.cfg", "cmd/my_menu-5-cvar2-2.cfg", "cmd/my_menu-5-cvar3-2.cfg",
    "cmd/my_menu-5-cvar5-2.cfg",
};

// One part of a menu command (between ';'): does it run on the server?
static int klxs_is_server_command(const char *part) {
    while (*part == ' ') part++;
    if (!*part || *part == '$' || *part == '_') return 0;           // empty, a variable, a menu alias
    static const char *const client[] = { "exec ", "exec\t", "touch_", "messagemode", "chooseteam", "yb ",
                                          "amx_", "play ", "vibrate", "echo", "alias ", "set ", "unalias" };
    for (size_t i = 0; i < sizeof client / sizeof *client; i++)
        if (!strncmp(part, client[i], strlen(client[i]))) return 0;
    return 1;
}

// `set _menu_cmd_N "a; b"` -> `set _menu_cmd_N "a; $avp_rc b"` for the parts that are server commands.
static void klxs_rewrite_cmd(const char *value, char *out, size_t cap) {
    size_t o = 0;
    const char *p = value;
    while (*p && o + 1 < cap) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char part[512];
        if (len >= sizeof part) len = sizeof part - 1;
        memcpy(part, p, len); part[len] = 0;
        const char *body = part; while (*body == ' ') body++;
        o += (size_t)snprintf(out + o, cap - o, "%s%s%s", klxs_is_server_command(part) ? "$avp_rc " : "", body, end ? "; " : "");
        if (o >= cap) { o = cap - 1; break; }
        if (!end) break;
        p = end + 1;
    }
    out[o < cap ? o : cap - 1] = 0;
}

static void klxs_mkdirs(const char *path) {
    char tmp[1600];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}

static int klxs_is_generated(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char first[160] = "";
    char *got = fgets(first, sizeof first, f);
    fclose(f);
    return got && !strcmp(first, KLXS_MARK);
}

static int klxs_write_menu(const char *src, const char *dst) {
    FILE *in = fopen(src, "r");
    if (!in) return -1;
    // A file the player put there himself is his; only our own copies are replaced.
    struct stat st;
    if (stat(dst, &st) == 0 && !klxs_is_generated(dst)) { fclose(in); return -1; }
    char (*lines)[640] = malloc(400 * sizeof *lines);
    if (!lines) { fclose(in); return -1; }
    int n = 0, type[64] = {0};
    while (n < 400 && fgets(lines[n], sizeof lines[n], in)) {
        int idx, t;
        if (sscanf(lines[n], " set _menu_type_%d %d", &idx, &t) == 2 && idx >= 0 && idx < 64) type[idx] = t;
        n++;
    }
    fclose(in);
    klxs_mkdirs(dst);
    FILE *f = fopen(dst, "w");
    if (!f) { free(lines); return -1; }
    fputs(KLXS_MARK, f);
    // Hosting (host_serverstate is 1 while loading, 2 while running) the commands run here; as a guest on a
    // server they go through rcon.
    fputs("cmd_scripting 1\nif $host_serverstate = 0\n:set avp_rc \"rcon\"\nelse\n:set avp_rc \"\"\n"
          "rcon_password " KLXS_PLACEHOLDER "\n", f);
    for (int i = 0; i < n; i++) {
        int idx;
        char value[512], word[64] = "";
        int is_cmd = sscanf(lines[i], " set _menu_cmd_%d \"%511[^\"]\"", &idx, value) == 2 && idx >= 0 && idx < 64;
        // A line the guest reads from the keyboard: a value field, or `messagemode <command>` as a button.
        int reads = 0;
        if (is_cmd && type[idx] == 3) reads = sscanf(value, " %63[A-Za-z0-9_]", word) == 1 && strlen(word) == strlen(value);
        else if (is_cmd) reads = sscanf(value, " messagemode %63[A-Za-z0-9_]", word) == 1;
        // (a helper the page keeps for itself – `bot_quota_value`, used by "bot_quota $bot_quota_value" – stays local)
        size_t wl = strlen(word);
        if (reads && wl > 6 && !strcmp(word + wl - 6, "_value")) reads = 0;
        if (is_cmd && reads) {
            const char *mm = type[idx] == 3 ? "" : "messagemode ";
            fprintf(f, "if $host_serverstate = 0\n:set _menu_cmd_%d \"%srcon; rcon_password " KLXS_PLACEHOLDER "+%s\"\nelse\n:set _menu_cmd_%d \"%s\"\n",
                    idx, mm, word, idx, value);
        } else if (is_cmd && type[idx] != 3) {
            char routed[900];
            klxs_rewrite_cmd(value, routed, sizeof routed);
            fprintf(f, "set _menu_cmd_%d \"%s\"\n", idx, routed);
        } else {
            fputs(lines[i], f);
            size_t len = strlen(lines[i]);
            if (len && lines[i][len - 1] != '\n') fputc('\n', f);
        }
    }
    fclose(f);
    free(lines);
    return 0;
}

static void klxs_menus(const char *gamepath, const char *assets, FILE *out) {
    int written = 0, removed = 0;
    for (size_t i = 0; i < sizeof k_menus / sizeof *k_menus; i++) {
        char src[1600], dst[1600];
        snprintf(src, sizeof src, "%s/touch/%s", assets ? assets : "", k_menus[i]);
        snprintf(dst, sizeof dst, "%s/touch/%s", gamepath, k_menus[i]);
        if (g_nadmin && assets) { if (klxs_write_menu(src, dst) == 0) written++; }
        else if (klxs_is_generated(dst) && unlink(dst) == 0) removed++;
    }
    if (out && written) fprintf(out, "  [xash] servers: %d menu pages route server commands through rcon on a remote server\n", written);
    if (out && removed) fprintf(out, "  [xash] servers: %d generated menu pages removed (no admin access listed)\n", removed);
}

// ---------------------------------------------------------------------------

void kl_xash_servers_prepare(const char *base, const char *gamedir, const char *assets, FILE *out) {
    if (!base || !*base || !gamedir || !*gamedir) return;
    char list[1400], gamepath[1400];
    snprintf(list, sizeof list, "%s/avpplay-servers.txt", base);
    snprintf(gamepath, sizeof gamepath, "%s/%s", base, gamedir);
    klxs_parse(list, out);
    klxs_favorites(gamepath, out);
    klxs_menus(gamepath, assets, out);
    if (!g_nadmin) return;
    // The placeholder goes where the guest's other start-up settings go (userconfig.cfg, written right after).
    const char *have = getenv("KL_XASH_CVARS");
    char cvars[2048];
    snprintf(cvars, sizeof cvars, "%s%srcon_password \"" KLXS_PLACEHOLDER "\"", have ? have : "", have && *have ? "; " : "");
    setenv("KL_XASH_CVARS", cvars, 1);
}

// ---------------------------------------------------------------------------
// 2. Admin access by destination

static void klxs_resolve(klxs_server *s) {
    if (s->resolved) return;
    s->resolved = -1;
    char host[128];
    snprintf(host, sizeof host, "%s", s->addr);
    char *colon = strrchr(host, ':');
    int port = 27015;
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return;
    s->ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    s->port = htons((uint16_t)port);
    s->resolved = 1;
    freeaddrinfo(res);
}

size_t kl_xash_rcon_rewrite(const void *buf, size_t n, const struct sockaddr *to, char *out, size_t cap) {
    static const char head[] = "\xff\xff\xff\xffrcon ";
    const size_t hl = sizeof head - 1;
    if (!g_nadmin || !to || n <= hl || memcmp(buf, head, hl)) return 0;
    uint32_t ip; uint16_t port;
    if (to->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)to;
        ip = a->sin_addr.s_addr; port = a->sin_port;
    } else if (to->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)to;
        if (!IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) return 0;
        memcpy(&ip, &a->sin6_addr.s6_addr[12], 4); port = a->sin6_port;
    } else return 0;
    const char *p = (const char *)buf + hl, *end = (const char *)buf + n;
    const char *sp = memchr(p, ' ', (size_t)(end - p));
    if (!sp) return 0;
    // Only the placeholder is replaced: a password the player set himself is his business. It may carry the
    // name of a setting ("avpplay+mp_timelimit"), which then goes in front of the value the player typed.
    const size_t phl = sizeof KLXS_PLACEHOLDER - 1;
    size_t tl = (size_t)(sp - p);
    if (tl < phl || memcmp(p, KLXS_PLACEHOLDER, phl)) return 0;
    char setting[72] = "";
    if (tl > phl) {
        if (p[phl] != '+' || tl - phl - 1 >= sizeof setting - 2) return 0;
        for (size_t i = phl + 1; i < tl; i++) if (!(isalnum((unsigned char)p[i]) || p[i] == '_')) return 0;
        setting[0] = ' ';
        memcpy(setting + 1, p + phl + 1, tl - phl - 1);
        setting[tl - phl] = 0;
    }
    for (int i = 0; i < g_nsrv; i++) {
        klxs_server *s = &g_srv[i];
        if (!s->password[0]) continue;
        klxs_resolve(s);
        if (s->resolved != 1 || s->ip != ip || s->port != port) continue;
        size_t pl = strlen(s->password), sl = strlen(setting), rest = (size_t)(end - sp);
        if (hl + pl + sl + rest > cap) return 0;
        memcpy(out, head, hl);
        memcpy(out + hl, s->password, pl);
        memcpy(out + hl + pl, setting, sl);
        memcpy(out + hl + pl + sl, sp, rest);
        return hl + pl + sl + rest;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 1. In the list

void kl_xash_servers_on_query(int fd, const void *buf, size_t n, const struct sockaddr *to, socklen_t tolen) {
    static const char xash[] = "\xff\xff\xff\xffinfo 49";
    static const char source[] = "\xff\xff\xff\xffTSource Engine Query";      // sent with its terminating NUL
    static const char scan[] = "1\xff" "0.0.0.0:0";                             // the engine's request to a master server
    if (!g_nsrv || !to || n < 9) return;
    int lan = n <= 64 && !memcmp(buf, xash, 9);
    int master = n >= sizeof scan - 1 && !memcmp(buf, scan, sizeof scan - 1);
    if (!lan && !master) return;
    if (to->sa_family != AF_INET && to->sa_family != AF_INET6) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + ts.tv_nsec / 1e9;
    for (int i = 0; i < g_nsrv; i++) {
        klxs_server *s = &g_srv[i];
        klxs_resolve(s);
        // One query per server per refresh: the guest sends a burst of them (one per built-in server).
        if (s->resolved != 1 || now - s->asked < 1.0) continue;
        s->asked = now;
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof ss);
        socklen_t len;
        if (to->sa_family == AF_INET) {
            struct sockaddr_in *a = (struct sockaddr_in *)&ss;
            a->sin_family = AF_INET; a->sin_addr.s_addr = s->ip; a->sin_port = s->port;
            len = sizeof *a;
        } else {
            // The guest's socket speaks IPv6; an IPv4 server is reached through its mapped address.
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
            a->sin6_family = AF_INET6; a->sin6_port = s->port;
            a->sin6_addr.s6_addr[10] = 0xff; a->sin6_addr.s6_addr[11] = 0xff;
            memcpy(&a->sin6_addr.s6_addr[12], &s->ip, 4);
            len = sizeof *a;
        }
#ifdef __APPLE__
        ((struct sockaddr *)&ss)->sa_len = (uint8_t)len;
#endif
        (void)tolen;
        if (s->goldsrc) sendto(fd, source, sizeof source, 0, (struct sockaddr *)&ss, len);
        else            sendto(fd, xash, sizeof xash - 1, 0, (struct sockaddr *)&ss, len);
    }
}

const char *kl_xash_servers_master_standin(const char *name) {
    static const char dead[] = "mentality.rip";
    if (!g_nsrv || !name) return NULL;
    size_t n = strlen(name), d = sizeof dead - 1;
    if (n < d || strcasecmp(name + n - d, dead) || (n > d && name[n - d - 1] != '.')) return NULL;
    if (!strncasecmp(name, "aaaa.", 5)) return NULL;       // the IPv6 names: one family is enough
    return "192.0.2.1";                                    // TEST-NET-1 (RFC 5737): routed nowhere
}
