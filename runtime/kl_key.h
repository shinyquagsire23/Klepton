//
// pthread_key_create, and what a caller does when it fails.
//
// A failed create leaves the key as it was: 0 for a static. pthread_getspecific
// does not check its key, and on Darwin slot 0 is not empty (it is libpthread's
// own), so a caller that carries on reads somebody else's pointer as its own
// per-thread state. The process has 512 slots and kl_x18_init walks up to slot
// 500 claiming its own (kl_x18.h), so running out is unlikely but not
// impossible. Every runtime key goes through here: it says which key failed,
// once, and returns 0 so the caller can take its own fallback (drop audio, no
// looper) or stop with that message instead of corrupting memory.
//

#ifndef KL_KEY_H
#define KL_KEY_H

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int kl_key_create(pthread_key_t *key, void (*dtor)(void *), const char *who) {
    int r = pthread_key_create(key, dtor);
    if (r == 0) return 1;
    fprintf(stderr, "  [klepton] pthread_key_create for %s failed: %s — the process "
                    "is out of thread-specific keys\n", who, strerror(r));
    return 0;
}

// For a key without which the caller cannot go on: stop here, with the reason,
// rather than run on a key that names another owner's slot.
static inline void kl_key_create_or_die(pthread_key_t *key, void (*dtor)(void *), const char *who) {
    if (!kl_key_create(key, dtor, who)) abort();
}

#endif
