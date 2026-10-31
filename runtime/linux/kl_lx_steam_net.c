// ISteamNetworkingSockets, for a game talking to itself.
//
// Source 2 is client/server even alone: the client and the listen server inside
// the same process exchange every message through a "connection", and on Steam
// that connection is an ISteamNetworkingSockets socket PAIR — two handles, each
// the other's peer, with no network anywhere. That one shape is all a
// single-player game uses of the interface, so it is all that is here: pairs,
// reliable in-order message queues between them, and poll groups. Everything
// that would reach another machine (listen sockets, P2P, relays, FakeIP) stays
// on the default slot, which names itself if the game ever calls it.
//
// Layouts are the Steamworks SDK's for these interface versions, under the
// 4-byte packing the SDK selects on Linux. They are written out as offsets
// rather than as structs, because the packing is the SDK's and not this
// compiler's: a field at the wrong offset here is the game reading a state or a
// length from the middle of something else.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kl_lx_steam_priv.h"

// SteamNetworkingMessage_t
enum { MSG_DATA = 0, MSG_SIZE = 8, MSG_CONN = 12, MSG_IDENTITY = 16, MSG_CONN_USERDATA = 152,
       MSG_TIME = 160, MSG_NUMBER = 168, MSG_FREE = 176, MSG_RELEASE = 184, MSG_CHANNEL = 192,
       MSG_FLAGS = 196, MSG_USERDATA = 200, MSG_LANE = 208, MSG_BYTES = 216 };
// SteamNetworkingIdentity: type, size, then 128 bytes of payload.
enum { IDENT_BYTES = 136, IDENT_TYPE_STEAMID = 16 };
// SteamNetConnectionInfo_t
enum { INFO_IDENTITY = 0, INFO_USERDATA = 136, INFO_STATE = 176, INFO_DESCRIPTION = 312,
       INFO_BYTES = 696 };
enum { STATE_NONE = 0, STATE_CONNECTED = 3, STATE_CLOSED_BY_PEER = 4 };
enum { RESULT_OK = 1, RESULT_INVALID_PARAM = 8, RESULT_NO_CONNECTION = 3 };
enum { AVAILABILITY_CURRENT = 100 };

#define W32(p, off) (*(uint32_t *)((uint8_t *)(p) + (off)))
#define W64(p, off) (*(uint64_t *)((uint8_t *)(p) + (off)))
#define WPTR(p, off) (*(void **)((uint8_t *)(p) + (off)))

typedef struct queue { void **msg; int head, n, cap; } queue;
typedef struct conn {
    uint32_t handle, peer, group;
    int      state;
    int64_t  userdata, sent;
    char     name[32];
    queue    in;
} conn;

#define NCONN 64
static conn g_conn[NCONN];
static queue g_group[NCONN];            // indexed by poll-group handle
static uint32_t g_ngroup;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static conn *find(uint32_t h) {
    for (int i = 0; i < NCONN; i++)
        if (h && g_conn[i].handle == h) return &g_conn[i];
    return NULL;
}

static void q_push(queue *q, void *m) {
    if (q->n == q->cap) {
        int cap = q->cap ? q->cap * 2 : 64;
        void **nm = malloc((size_t)cap * sizeof *nm);
        for (int i = 0; i < q->n; i++) nm[i] = q->msg[(q->head + i) % q->cap];
        free(q->msg);
        q->msg = nm; q->head = 0; q->cap = cap;
    }
    q->msg[(q->head + q->n++) % q->cap] = m;
}
static void *q_pop(queue *q) {
    if (!q->n) return NULL;
    void *m = q->msg[q->head];
    q->head = (q->head + 1) % q->cap;
    q->n--;
    return m;
}

static int64_t now_usec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // Never near zero: the SDK promises a large initial value so that "0" can
    // mean "never", and callers subtract timeouts from it.
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 + 30ll * 24 * 3600 * 1000000;
}

static void identity_self(void *out) {
    memset(out, 0, IDENT_BYTES);
    W32(out, 0) = IDENT_TYPE_STEAMID;
    W32(out, 4) = 8;
    W64(out, 8) = LX_STEAM_ID;
}

// ---- messages ----
// The guest calls these two through the pointers in the message, so they are
// part of the ABI as much as the vtable is.
static void msg_free_data(void *m) { free(WPTR(m, MSG_DATA)); }
static void msg_release(void *m) {
    void (*fd)(void *) = (void (*)(void *))WPTR(m, MSG_FREE);
    if (fd) fd(m);
    free(m);
}
static void *msg_new(int bytes) {
    void *m = calloc(1, MSG_BYTES);
    if (bytes > 0) {
        WPTR(m, MSG_DATA) = malloc((size_t)bytes);
        WPTR(m, MSG_FREE) = (void *)msg_free_data;
    }
    W32(m, MSG_SIZE) = (uint32_t)(bytes > 0 ? bytes : 0);
    WPTR(m, MSG_RELEASE) = (void *)msg_release;
    return m;
}

// Hand a message to the other end of `from`. Called with the lock held.
static int64_t deliver(conn *from, void *m) {
    conn *to = find(from->peer);
    if (!to || to->state != STATE_CONNECTED) return -1;
    W32(m, MSG_CONN) = to->handle;
    W64(m, MSG_CONN_USERDATA) = (uint64_t)to->userdata;
    W64(m, MSG_TIME) = (uint64_t)now_usec();
    W64(m, MSG_NUMBER) = (uint64_t)++from->sent;
    identity_self((uint8_t *)m + MSG_IDENTITY);
    q_push(to->group ? &g_group[to->group] : &to->in, m);
    return from->sent;
}

// ---- ISteamNetworkingSockets ----
static uint32_t ns_CreateSocketPair(lx_iface *s, uint32_t *out1, uint32_t *out2, uint32_t loopback,
                                    const void *id1, const void *id2) {
    (void)s; (void)loopback; (void)id1; (void)id2;
    static uint32_t next = 0x10000;
    pthread_mutex_lock(&g_lock);
    conn *a = NULL, *b = NULL;
    for (int i = 0; i < NCONN && !b; i++)
        if (!g_conn[i].handle) { if (!a) a = &g_conn[i]; else b = &g_conn[i]; }
    if (b) {
        memset(a, 0, sizeof *a); memset(b, 0, sizeof *b);
        a->handle = ++next; b->handle = ++next;
        a->peer = b->handle; b->peer = a->handle;
        a->state = b->state = STATE_CONNECTED;
        *out1 = a->handle; *out2 = b->handle;
    }
    pthread_mutex_unlock(&g_lock);
    if (b) STEAM_LOG("CreateSocketPair: %#x <-> %#x\n", a->handle, b->handle);
    return b != NULL;
}

static uint32_t ns_CloseConnection(lx_iface *s, uint32_t h, int reason, const char *dbg, uint32_t linger) {
    (void)s; (void)reason; (void)dbg; (void)linger;
    pthread_mutex_lock(&g_lock);
    conn *c = find(h), *p = c ? find(c->peer) : NULL;
    if (c) {
        for (void *m; (m = q_pop(&c->in)); ) msg_release(m);
        free(c->in.msg);
        if (p) p->state = STATE_CLOSED_BY_PEER;
        memset(c, 0, sizeof *c);
    }
    pthread_mutex_unlock(&g_lock);
    return c != NULL;
}

static uint32_t ns_SetConnectionUserData(lx_iface *s, uint32_t h, int64_t ud) {
    (void)s;
    pthread_mutex_lock(&g_lock);
    conn *c = find(h);
    if (c) c->userdata = ud;
    pthread_mutex_unlock(&g_lock);
    return c != NULL;
}
static int64_t ns_GetConnectionUserData(lx_iface *s, uint32_t h) {
    (void)s;
    conn *c = find(h);
    return c ? c->userdata : -1;
}
static void ns_SetConnectionName(lx_iface *s, uint32_t h, const char *name) {
    (void)s;
    conn *c = find(h);
    if (c && name) snprintf(c->name, sizeof c->name, "%s", name);
}
static uint32_t ns_GetConnectionName(lx_iface *s, uint32_t h, char *out, int cap) {
    (void)s;
    conn *c = find(h);
    if (!c || cap <= 0) return 0;
    snprintf(out, (size_t)cap, "%s", c->name);
    return 1;
}

static int32_t ns_SendMessageToConnection(lx_iface *s, uint32_t h, const void *data, uint32_t n,
                                          int flags, int64_t *out_number) {
    (void)s;
    pthread_mutex_lock(&g_lock);
    conn *c = find(h);
    int32_t r = RESULT_INVALID_PARAM;
    if (c) {
        void *m = msg_new((int)n);
        if (n) memcpy(WPTR(m, MSG_DATA), data, n);
        W32(m, MSG_FLAGS) = (uint32_t)flags;
        int64_t num = deliver(c, m);
        if (num < 0) { msg_release(m); r = RESULT_NO_CONNECTION; }
        else { if (out_number) *out_number = num; r = RESULT_OK; }
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

// Takes ownership of every message, as the SDK does, sent or not.
static void ns_SendMessages(lx_iface *s, int n, void *const *msgs, int64_t *out) {
    (void)s;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < n; i++) {
        conn *c = find(W32(msgs[i], MSG_CONN));
        int64_t num = c ? deliver(c, msgs[i]) : -1;
        if (num < 0) msg_release(msgs[i]);
        if (out) out[i] = num < 0 ? -(int64_t)RESULT_NO_CONNECTION : num;
    }
    pthread_mutex_unlock(&g_lock);
}

static int32_t receive(queue *q, void **out, int max) {
    int n = 0;
    pthread_mutex_lock(&g_lock);
    while (n < max && q->n) out[n++] = q_pop(q);
    pthread_mutex_unlock(&g_lock);
    return n;
}
static int32_t ns_ReceiveMessagesOnConnection(lx_iface *s, uint32_t h, void **out, int max) {
    (void)s;
    conn *c = find(h);
    return c ? receive(&c->in, out, max) : -1;
}
static int32_t ns_ReceiveMessagesOnPollGroup(lx_iface *s, uint32_t g, void **out, int max) {
    (void)s;
    return g && g < NCONN ? receive(&g_group[g], out, max) : -1;
}

static uint32_t ns_GetConnectionInfo(lx_iface *s, uint32_t h, void *info) {
    (void)s;
    conn *c = find(h);
    if (!c || !info) return 0;
    memset(info, 0, INFO_BYTES);
    identity_self((uint8_t *)info + INFO_IDENTITY);
    W64(info, INFO_USERDATA) = (uint64_t)c->userdata;
    W32(info, INFO_STATE) = (uint32_t)c->state;
    snprintf((char *)info + INFO_DESCRIPTION, 128, "#%u pipe %s", c->handle, c->name);
    return 1;
}

// SteamNetConnectionRealTimeStatus_t: state, ping, six floats, four ints, a
// queue time and reserve — 120 bytes. A perfect link with nothing queued.
static int32_t ns_GetConnectionRealTimeStatus(lx_iface *s, uint32_t h, void *st, int nlanes, void *lanes) {
    (void)s;
    conn *c = find(h);
    if (!c) return RESULT_NO_CONNECTION;
    if (st) {
        memset(st, 0, 120);
        W32(st, 0) = (uint32_t)c->state;
        float one = 1.0f;
        memcpy((uint8_t *)st + 8, &one, 4);
        memcpy((uint8_t *)st + 12, &one, 4);
        W32(st, 32) = 64 * 1024 * 1024;             // send rate, bytes per second
    }
    if (lanes && nlanes > 0) memset(lanes, 0, (size_t)nlanes * 32);
    return RESULT_OK;
}

static uint32_t ns_GetIdentity(lx_iface *s, void *out) { (void)s; if (out) identity_self(out); return 1; }
static int32_t  ns_ok(lx_iface *s)                      { (void)s; return RESULT_OK; }
static uint32_t ns_true(lx_iface *s)                    { (void)s; return 1; }
static int32_t  ns_available(lx_iface *s, void *details) {
    (void)s;
    if (details) { memset(details, 0, 4 + 256); W32(details, 0) = AVAILABILITY_CURRENT; }
    return AVAILABILITY_CURRENT;
}
static int32_t  ns_InitAuthentication(lx_iface *s)      { (void)s; return AVAILABILITY_CURRENT; }
static void     ns_void(lx_iface *s)                    { (void)s; }

static uint32_t ns_CreatePollGroup(lx_iface *s) {
    (void)s;
    pthread_mutex_lock(&g_lock);
    uint32_t g = g_ngroup + 1 < NCONN ? ++g_ngroup : 0;
    pthread_mutex_unlock(&g_lock);
    return g;
}
static uint32_t ns_SetConnectionPollGroup(lx_iface *s, uint32_t h, uint32_t g) {
    (void)s;
    pthread_mutex_lock(&g_lock);
    conn *c = find(h);
    if (c && g < NCONN) {
        // What is already waiting on the connection moves with it.
        queue *to = g ? &g_group[g] : NULL;
        if (to) for (void *m; (m = q_pop(&c->in)); ) q_push(to, m);
        c->group = g;
    }
    pthread_mutex_unlock(&g_lock);
    return c != NULL;
}

void kl_lx_steam_net_sockets(lx_iface *i) {
    void **t = i->table;
    t[5]  = ns_CloseConnection;
    t[7]  = ns_SetConnectionUserData;
    t[8]  = ns_GetConnectionUserData;
    t[9]  = ns_SetConnectionName;
    t[10] = ns_GetConnectionName;
    t[11] = ns_SendMessageToConnection;
    t[12] = ns_SendMessages;
    t[13] = ns_ok;                      // FlushMessagesOnConnection
    t[14] = ns_ReceiveMessagesOnConnection;
    t[15] = ns_GetConnectionInfo;
    t[16] = ns_GetConnectionRealTimeStatus;
    t[19] = ns_CreateSocketPair;
    t[20] = ns_ok;                      // ConfigureConnectionLanes
    t[21] = ns_GetIdentity;
    t[22] = ns_InitAuthentication;
    t[23] = ns_available;               // GetAuthenticationStatus
    t[24] = ns_CreatePollGroup;
    t[25] = ns_true;                    // DestroyPollGroup
    t[26] = ns_SetConnectionPollGroup;
    t[27] = ns_ReceiveMessagesOnPollGroup;
    t[41] = ns_void;                    // RunCallbacks
}

// ---- ISteamNetworkingUtils ----
static void    *nu_AllocateMessage(lx_iface *s, int bytes)       { (void)s; return msg_new(bytes); }
static int32_t  nu_GetRelayNetworkStatus(lx_iface *s, void *d)   {
    (void)s;
    if (d) { memset(d, 0, 16 + 256); W32(d, 0) = W32(d, 8) = W32(d, 12) = AVAILABILITY_CURRENT; }
    return AVAILABILITY_CURRENT;
}
static int64_t  nu_GetLocalTimestamp(lx_iface *s)                { (void)s; return now_usec(); }

void kl_lx_steam_net_utils(lx_iface *i) {
    void **t = i->table;
    t[0]  = nu_AllocateMessage;
    t[1]  = nu_GetRelayNetworkStatus;
    t[7]  = ns_true;                    // CheckPingDataUpToDate
    t[12] = nu_GetLocalTimestamp;
    t[13] = ns_void;                    // SetDebugOutputFunction
    t[16] = ns_true;                    // SetConfigValue: accepted, and without effect
}
