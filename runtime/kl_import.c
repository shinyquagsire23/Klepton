// kl_import — see kl_import.h.
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "klepton.h"
#include "kl_import.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_frontend;
static int g_next = 1;
static struct {
    int ticket;     // 0: none
    int shown;      // taken by the frontend
    int status;     // KL_IMPORT_*
    char title[160], exts[160], dest[1024];
} g_req;

void kl_import_frontend(int available) {
    pthread_mutex_lock(&g_lock);
    g_frontend = available;
    pthread_mutex_unlock(&g_lock);
}

int klepton_import_file(const char *title, const char *extensions, const char *dest) {
    if (!dest || !*dest) return 0;
    char host[1024];
    const char *hp = kl_guest_path(dest, host, sizeof host);
    int ticket = 0;
    pthread_mutex_lock(&g_lock);
    const int busy = g_req.ticket && g_req.status == KL_IMPORT_PENDING;
    if (g_frontend && hp && !busy) {
        ticket = g_next++;
        g_req.ticket = ticket;
        g_req.shown = 0;
        g_req.status = KL_IMPORT_PENDING;
        snprintf(g_req.title, sizeof g_req.title, "%s", title ? title : "Choose a file");
        snprintf(g_req.exts, sizeof g_req.exts, "%s", extensions ? extensions : "");
        snprintf(g_req.dest, sizeof g_req.dest, "%s", hp);
    }
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "  [import] file for %s: %s\n", dest,
            ticket ? "picker asked" : !g_frontend ? "no picker here" : busy ? "a picker is already up" : "bad path");
    return ticket;
}

int klepton_import_status(int ticket) {
    pthread_mutex_lock(&g_lock);
    const int s = ticket > 0 && ticket == g_req.ticket ? g_req.status : KL_IMPORT_UNKNOWN;
    pthread_mutex_unlock(&g_lock);
    return s;
}

int kl_import_take(char *title, size_t tcap, char *exts, size_t ecap, char *dest, size_t dcap) {
    int ticket = 0;
    pthread_mutex_lock(&g_lock);
    if (g_req.ticket && !g_req.shown && g_req.status == KL_IMPORT_PENDING) {
        g_req.shown = 1;
        ticket = g_req.ticket;
        if (title) snprintf(title, tcap, "%s", g_req.title);
        if (exts) snprintf(exts, ecap, "%s", g_req.exts);
        if (dest) snprintf(dest, dcap, "%s", g_req.dest);
    }
    pthread_mutex_unlock(&g_lock);
    return ticket;
}

void kl_import_finish(int ticket, int status) {
    pthread_mutex_lock(&g_lock);
    if (ticket == g_req.ticket && g_req.status == KL_IMPORT_PENDING) g_req.status = status;
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "  [import] ticket %d: %s\n", ticket,
            status == KL_IMPORT_COPIED ? "copied" : status == KL_IMPORT_CANCELLED ? "cancelled" : "failed");
}
