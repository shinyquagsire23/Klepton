// OpenSL ES, enough of it for Unity's FMOD to open an output device.
//
// Measured rather than guessed: serving the library handle and logging dlsym
// showed FMOD wants exactly six symbols — slCreateEngine, and the interface ids
// SL_IID_{ENGINE,ANDROIDSIMPLEBUFFERQUEUE,ANDROIDCONFIGURATION,PLAY,RECORD}.
// Everything else still resolves to a trampoline that aborts naming itself.
//
// OpenSL ES is COM-shaped: an object is a pointer to a pointer to a vtable, and
// every method takes that same pointer back as its first argument. So each
// object here is a struct whose *first* member is its vtable pointer, and the
// handle we hand out is just the struct address. The vtable layouts below are
// the spec's, in the spec's order — the guest indexes them by position, so an
// entry in the wrong slot is a call to the wrong function with no diagnostic
// whatsoever.
//
// What this does NOT do is make a sound. The player consumes buffers on a
// thread at real-time rate and calls the buffer-queue callback back, which is
// all FMOD needs to keep its mixer running and stop the guest aborting. Binding
// it to CoreAudio is a separate, later job — the point of this file is to get
// past audio to the renderer.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "klepton.h"
#include "kl_opensl.h"

typedef uint32_t SLuint32;
typedef int32_t  SLresult;
typedef uint32_t SLboolean;

#define SL_RESULT_SUCCESS            0
#define SL_RESULT_FEATURE_UNSUPPORTED 12
#define SL_BOOLEAN_FALSE             0
#define SL_OBJECT_STATE_REALIZED     2
#define SL_PLAYSTATE_STOPPED         1
#define SL_PLAYSTATE_PAUSED          2
#define SL_PLAYSTATE_PLAYING         3

// The 128-bit interface id. FMOD passes these straight through from dlsym to
// GetInterface, so identity is what matters; the contents are here so that a
// caller which compares by value rather than by pointer still tells them apart.
typedef struct SLInterfaceID_ {
    uint32_t time_low; uint16_t time_mid, time_hi, clock_seq; uint8_t node[6];
} *SLInterfaceID;

#define IID(sym, tag) \
    static const struct SLInterfaceID_ sym##_data = {tag, 0, 0, 0, {0,0,0,0,0,0}}; \
    static const struct SLInterfaceID_ *sym = &sym##_data;
IID(SL_IID_ENGINE,                 0x1001)
IID(SL_IID_PLAY,                   0x1002)
IID(SL_IID_ANDROIDSIMPLEBUFFERQUEUE, 0x1003)
IID(SL_IID_ANDROIDCONFIGURATION,   0x1004)
IID(SL_IID_RECORD,                 0x1005)
IID(SL_IID_BUFFERQUEUE,            0x1006)
IID(SL_IID_VOLUME,                 0x1007)

// ---- vtable layouts, in spec order ----
struct SLObjectItf_;
typedef const struct SLObjectItf_ *const *SLObjectItf;
struct SLObjectItf_ {
    SLresult (*Realize)(SLObjectItf, SLboolean);
    SLresult (*Resume)(SLObjectItf, SLboolean);
    SLresult (*GetState)(SLObjectItf, SLuint32 *);
    SLresult (*GetInterface)(SLObjectItf, const SLInterfaceID, void *);
    SLresult (*RegisterCallback)(SLObjectItf, void *, void *);
    void     (*AbortAsyncOperation)(SLObjectItf);
    void     (*Destroy)(SLObjectItf);
    SLresult (*SetPriority)(SLObjectItf, int32_t, SLboolean);
    SLresult (*GetPriority)(SLObjectItf, int32_t *);
    SLresult (*SetLossOfControlInterfaces)(SLObjectItf, int16_t, SLInterfaceID *, SLboolean);
};

struct SLEngineItf_;
typedef const struct SLEngineItf_ *const *SLEngineItf;
struct SLEngineItf_ {
    SLresult (*CreateLEDDevice)(SLEngineItf, SLObjectItf *, SLuint32, SLuint32,
                                const SLInterfaceID *, const SLboolean *);
    SLresult (*CreateVibraDevice)(SLEngineItf, SLObjectItf *, SLuint32, SLuint32,
                                  const SLInterfaceID *, const SLboolean *);
    SLresult (*CreateAudioPlayer)(SLEngineItf, SLObjectItf *, void *, void *,
                                  SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (*CreateAudioRecorder)(SLEngineItf, SLObjectItf *, void *, void *,
                                    SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (*CreateMidiPlayer)(SLEngineItf, SLObjectItf *, void *, void *, void *,
                                 void *, void *, SLuint32, const SLInterfaceID *,
                                 const SLboolean *);
    SLresult (*CreateListener)(SLEngineItf, SLObjectItf *, SLuint32,
                               const SLInterfaceID *, const SLboolean *);
    SLresult (*Create3DGroup)(SLEngineItf, SLObjectItf *, SLuint32,
                              const SLInterfaceID *, const SLboolean *);
    SLresult (*CreateOutputMix)(SLEngineItf, SLObjectItf *, SLuint32,
                                const SLInterfaceID *, const SLboolean *);
    SLresult (*CreateMetadataExtractor)(SLEngineItf, SLObjectItf *, void *, SLuint32,
                                        const SLInterfaceID *, const SLboolean *);
    SLresult (*CreateExtensionObject)(SLEngineItf, SLObjectItf *, void *, SLuint32,
                                      SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (*QueryNumSupportedInterfaces)(SLEngineItf, SLuint32, SLuint32 *);
    SLresult (*QuerySupportedInterfaces)(SLEngineItf, SLuint32, SLuint32, SLInterfaceID *);
    SLresult (*QueryNumSupportedExtensions)(SLEngineItf, SLuint32 *);
    SLresult (*QuerySupportedExtension)(SLEngineItf, SLuint32, char *, int16_t *);
    SLresult (*IsExtensionSupported)(SLEngineItf, const char *, SLboolean *);
};

struct SLPlayItf_;
typedef const struct SLPlayItf_ *const *SLPlayItf;
struct SLPlayItf_ {
    SLresult (*SetPlayState)(SLPlayItf, SLuint32);
    SLresult (*GetPlayState)(SLPlayItf, SLuint32 *);
    SLresult (*GetDuration)(SLPlayItf, SLuint32 *);
    SLresult (*GetPosition)(SLPlayItf, SLuint32 *);
    SLresult (*RegisterCallback)(SLPlayItf, void *, void *);
    SLresult (*SetCallbackEventsMask)(SLPlayItf, SLuint32);
    SLresult (*GetCallbackEventsMask)(SLPlayItf, SLuint32 *);
    SLresult (*SetMarkerPosition)(SLPlayItf, SLuint32);
    SLresult (*ClearMarkerPosition)(SLPlayItf);
    SLresult (*GetMarkerPosition)(SLPlayItf, SLuint32 *);
    SLresult (*SetPositionUpdatePeriod)(SLPlayItf, SLuint32);
    SLresult (*GetPositionUpdatePeriod)(SLPlayItf, SLuint32 *);
};

struct SLBufferQueueItf_;
typedef const struct SLBufferQueueItf_ *const *SLBufferQueueItf;
typedef struct { SLuint32 count, index; } SLBufferQueueState;
struct SLBufferQueueItf_ {
    SLresult (*Enqueue)(SLBufferQueueItf, const void *, SLuint32);
    SLresult (*Clear)(SLBufferQueueItf);
    SLresult (*GetState)(SLBufferQueueItf, SLBufferQueueState *);
    SLresult (*RegisterCallback)(SLBufferQueueItf, void (*)(SLBufferQueueItf, void *), void *);
};

struct SLConfigItf_;
typedef const struct SLConfigItf_ *const *SLConfigItf;
struct SLConfigItf_ {
    SLresult (*SetConfiguration)(SLConfigItf, const char *, const void *, SLuint32);
    SLresult (*GetConfiguration)(SLConfigItf, const char *, SLuint32 *, void *);
};

// SLDataFormat_PCM. samplesPerSec is in *milliHz*, which is the kind of detail
// that silently produces a player running 1000x too slow.
typedef struct {
    SLuint32 formatType, numChannels, samplesPerSec, bitsPerSample,
             containerSize, channelMask, endianness;
} SLDataFormat_PCM;
typedef struct { void *pLocator; void *pFormat; } SLDataSource;

#define SL_DATAFORMAT_PCM 2

// ---- objects ----
#define KL_SL_QUEUE 16

typedef struct kl_sl_player {
    const struct SLObjectItf_ *obj_vt;      // must stay first: this IS the handle
    const struct SLPlayItf_   *play_vt;
    const struct SLBufferQueueItf_ *bq_vt;
    const struct SLConfigItf_ *cfg_vt;

    pthread_mutex_t lock;
    pthread_cond_t  wake;
    pthread_cond_t  cb_done;                // signalled when in_cb drops to 0
    pthread_t       thread;
    int             running, started;
    int             in_cb;                  // a guest callback is on the stack
    SLuint32        state;

    void (*cb)(SLBufferQueueItf, void *);
    void *cb_ctx;

    struct { const void *buf; SLuint32 size; } q[KL_SL_QUEUE];
    unsigned head, tail, count;
    unsigned generation;                    // bumped on every re-creation

    unsigned rate, channels, bits;          // from the PCM format
    unsigned long consumed;
} kl_sl_player;

typedef struct { const struct SLObjectItf_ *obj_vt; } kl_sl_mix;
typedef struct {
    const struct SLObjectItf_ *obj_vt;
    const struct SLEngineItf_ *engine_vt;
} kl_sl_engine;

static kl_sl_engine g_engine;
static kl_sl_mix    g_mix;
static kl_sl_player g_player;
static unsigned long g_buffers_consumed;

static void player_stop(kl_sl_player *p);
static void player_wait_cb(kl_sl_player *p);   // caller must hold p->lock

// ---- the feeder ----
//
// FMOD enqueues a buffer, we "play" it for as long as it would really take, then
// call back so it enqueues the next. That callback runs guest code on this
// thread, so kl_thread_init() is mandatory before the first one (S0.1) — without
// it the stack-protector prologue reads an empty TSD slot and the guest dies far
// from here.
static void *feeder(void *arg) {
    kl_sl_player *p = arg;
    kl_thread_init();
    pthread_mutex_lock(&p->lock);
    while (p->running) {
        if (p->state != SL_PLAYSTATE_PLAYING || p->count == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 5 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&p->wake, &p->lock, &ts);
            continue;
        }
        SLuint32 size = p->q[p->head].size;
        p->head = (p->head + 1) % KL_SL_QUEUE;
        p->count--;
        unsigned frame = (p->channels * p->bits) / 8;
        if (!frame) frame = 4;
        unsigned rate = p->rate ? p->rate : 48000;
        useconds_t us = (useconds_t)((uint64_t)(size / frame) * 1000000ull / rate);
        unsigned generation = p->generation;
        pthread_mutex_unlock(&p->lock);

        if (us) usleep(us > 100000 ? 100000 : us);   // cap, so a bogus size cannot wedge us

        // Re-check *after* the sleep and take the callback then, not before it.
        //
        // Snapshotting the callback before sleeping is not enough, and this cost a
        // crash that looked like it belonged to something else entirely. A buffer's
        // worth of sleep is milliseconds during which the guest can call
        // SetPlayState(STOPPED) — which is exactly what Unity does when XR comes up
        // and it reconfigures audio. Real OpenSL stops delivering buffer-queue
        // callbacks at that point; we went on to call one, and FMOD, having already
        // released its mixer buffers, took a memmove through a null base + offset.
        // It presented as a fault in _platform_memmove on a guest worker thread,
        // three layers from the mistake, and it was previously read as a missing
        // Oculus spatializer rather than as our own race.
        pthread_mutex_lock(&p->lock);
        if (!p->running || generation != p->generation) break;
        if (p->state != SL_PLAYSTATE_PLAYING) continue;   // stopped or paused mid-buffer
        void (*cb)(SLBufferQueueItf, void *) = p->cb;
        void *cb_ctx = p->cb_ctx;
        // Held across the callback so a concurrent SetPlayState(STOPPED) or
        // Destroy waits for it to return rather than freeing underneath it.
        p->in_cb = 1;
        pthread_mutex_unlock(&p->lock);

        p->consumed++;
        g_buffers_consumed++;
        if (cb) cb((SLBufferQueueItf)&p->bq_vt, cb_ctx);

        pthread_mutex_lock(&p->lock);
        p->in_cb = 0;
        pthread_cond_broadcast(&p->cb_done);
        // If the device was torn down while we were outside the lock, this
        // thread is the previous generation and must not touch the queue again.
        if (generation != p->generation) break;
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

// ---- SLObjectItf ----
static SLresult obj_Realize(SLObjectItf self, SLboolean async) {
    (void)async;
    if ((void *)self == (void *)&g_player) {
        kl_sl_player *p = &g_player;
        if (!p->started) {
            p->started = p->running = 1;
            pthread_create(&p->thread, NULL, feeder, p);
        }
    }
    return SL_RESULT_SUCCESS;
}
static SLresult obj_Resume(SLObjectItf s, SLboolean a) { (void)s; (void)a; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(SLObjectItf s, SLuint32 *st) {
    (void)s; if (st) *st = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS;
}
static SLresult obj_RegisterCallback(SLObjectItf s, void *cb, void *ctx) {
    (void)s; (void)cb; (void)ctx; return SL_RESULT_SUCCESS;
}
static void obj_AbortAsyncOperation(SLObjectItf s) { (void)s; }
static void obj_Destroy(SLObjectItf self) {
    if ((void *)self == (void *)&g_player) player_stop(&g_player);
}
static SLresult obj_SetPriority(SLObjectItf s, int32_t p, SLboolean e) {
    (void)s; (void)p; (void)e; return SL_RESULT_SUCCESS;
}
static SLresult obj_GetPriority(SLObjectItf s, int32_t *p) {
    (void)s; if (p) *p = 0; return SL_RESULT_SUCCESS;
}
static SLresult obj_SetLossOfControl(SLObjectItf s, int16_t n, SLInterfaceID *i, SLboolean e) {
    (void)s; (void)n; (void)i; (void)e; return SL_RESULT_SUCCESS;
}

static SLresult obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *out);

static const struct SLObjectItf_ g_obj_vt = {
    obj_Realize, obj_Resume, obj_GetState, obj_GetInterface, obj_RegisterCallback,
    obj_AbortAsyncOperation, obj_Destroy, obj_SetPriority, obj_GetPriority,
    obj_SetLossOfControl,
};

// ---- SLPlayItf ----
static SLresult play_SetPlayState(SLPlayItf self, SLuint32 state) {
    (void)self;
    pthread_mutex_lock(&g_player.lock);
    g_player.state = state;
    pthread_cond_signal(&g_player.wake);
    // Stopping is synchronous: do not return until the guest's callback is off
    // the stack, or it will run against buffers the guest is about to release.
    if (state != SL_PLAYSTATE_PLAYING) player_wait_cb(&g_player);
    pthread_mutex_unlock(&g_player.lock);
    fprintf(stderr, "  [sl] play state -> %s\n",
            state == SL_PLAYSTATE_PLAYING ? "PLAYING" :
            state == SL_PLAYSTATE_PAUSED  ? "PAUSED" : "STOPPED");
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(SLPlayItf s, SLuint32 *st) {
    (void)s; if (st) *st = g_player.state; return SL_RESULT_SUCCESS;
}
static SLresult play_GetU32(SLPlayItf s, SLuint32 *v) { (void)s; if (v) *v = 0; return SL_RESULT_SUCCESS; }
static SLresult play_Reg(SLPlayItf s, void *c, void *x) { (void)s; (void)c; (void)x; return SL_RESULT_SUCCESS; }
static SLresult play_SetU32(SLPlayItf s, SLuint32 v) { (void)s; (void)v; return SL_RESULT_SUCCESS; }
static SLresult play_Clear(SLPlayItf s) { (void)s; return SL_RESULT_SUCCESS; }

static const struct SLPlayItf_ g_play_vt = {
    play_SetPlayState, play_GetPlayState, play_GetU32, play_GetU32, play_Reg,
    play_SetU32, play_GetU32, play_SetU32, play_Clear, play_GetU32,
    play_SetU32, play_GetU32,
};

// ---- SLAndroidSimpleBufferQueueItf ----
static SLresult bq_Enqueue(SLBufferQueueItf self, const void *buf, SLuint32 size) {
    (void)self;
    pthread_mutex_lock(&g_player.lock);
    if (g_player.count == KL_SL_QUEUE) {
        pthread_mutex_unlock(&g_player.lock);
        return SL_RESULT_FEATURE_UNSUPPORTED;   // queue full; FMOD retries
    }
    g_player.q[g_player.tail].buf = buf;
    g_player.q[g_player.tail].size = size;
    g_player.tail = (g_player.tail + 1) % KL_SL_QUEUE;
    g_player.count++;
    pthread_cond_signal(&g_player.wake);
    pthread_mutex_unlock(&g_player.lock);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_Clear(SLBufferQueueItf self) {
    (void)self;
    pthread_mutex_lock(&g_player.lock);
    g_player.head = g_player.tail = g_player.count = 0;
    pthread_mutex_unlock(&g_player.lock);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(SLBufferQueueItf self, SLBufferQueueState *st) {
    (void)self;
    if (st) { st->count = g_player.count; st->index = (SLuint32)g_player.consumed; }
    return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(SLBufferQueueItf self,
                                    void (*cb)(SLBufferQueueItf, void *), void *ctx) {
    (void)self;
    g_player.cb = cb;
    g_player.cb_ctx = ctx;
    return SL_RESULT_SUCCESS;
}
static const struct SLBufferQueueItf_ g_bq_vt = {
    bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback,
};

// ---- SLAndroidConfigurationItf ----
static SLresult cfg_Set(SLConfigItf s, const char *k, const void *v, SLuint32 n) {
    (void)s; (void)v; (void)n;
    fprintf(stderr, "  [sl] SetConfiguration(\"%s\") — recorded, not applied\n",
            k ? k : "?");
    return SL_RESULT_SUCCESS;
}
static SLresult cfg_Get(SLConfigItf s, const char *k, SLuint32 *n, void *v) {
    (void)s; (void)k; (void)v; if (n) *n = 0; return SL_RESULT_FEATURE_UNSUPPORTED;
}
static const struct SLConfigItf_ g_cfg_vt = { cfg_Set, cfg_Get };

// ---- SLEngineItf ----
static SLresult eng_CreateOutputMix(SLEngineItf self, SLObjectItf *out, SLuint32 n,
                                    const SLInterfaceID *ids, const SLboolean *req) {
    (void)self; (void)n; (void)ids; (void)req;
    g_mix.obj_vt = &g_obj_vt;
    if (out) *out = (SLObjectItf)&g_mix;
    return SL_RESULT_SUCCESS;
}

// Stop and join the feeder, if one is running. Must happen before the player is
// reinitialised: FMOD destroys and re-creates the device, and memset-ing the
// struct out from under a live feeder zeroes the mutex it is holding and the
// callback it is about to call. That crashed inside FMOD's callback as a
// memmove through a null pointer, several layers from the actual mistake.
static void player_stop(kl_sl_player *p) {
    if (!p->started) return;
    pthread_mutex_lock(&p->lock);
    p->running = 0;
    p->generation++;
    pthread_cond_signal(&p->wake);
    pthread_mutex_unlock(&p->lock);
    pthread_join(p->thread, NULL);
    p->started = 0;
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->wake);
    pthread_cond_destroy(&p->cb_done);
}

// Wait for any buffer-queue callback already on the stack to return. Real OpenSL
// makes a stop synchronous with respect to callbacks, so the guest is entitled to
// free what the callback touches the moment this returns.
//
// The self-check is not hypothetical politeness: the guest may well call
// SetPlayState from inside the callback, and waiting for ourselves there would
// hang rather than crash — which is the harder failure to read (trap 5).
static void player_wait_cb(kl_sl_player *p) {
    if (p->started && !pthread_equal(pthread_self(), p->thread))
        while (p->in_cb) pthread_cond_wait(&p->cb_done, &p->lock);
}

static SLresult eng_CreateAudioPlayer(SLEngineItf self, SLObjectItf *out,
                                      void *src, void *snk, SLuint32 n,
                                      const SLInterfaceID *ids, const SLboolean *req) {
    (void)self; (void)snk; (void)n; (void)ids; (void)req;
    kl_sl_player *p = &g_player;
    player_stop(p);
    unsigned gen = p->generation;
    memset(p, 0, sizeof *p);
    p->generation = gen;
    p->obj_vt = &g_obj_vt;
    p->play_vt = &g_play_vt;
    p->bq_vt = &g_bq_vt;
    p->cfg_vt = &g_cfg_vt;
    p->state = SL_PLAYSTATE_STOPPED;
    p->rate = 48000; p->channels = 2; p->bits = 16;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->wake, NULL);
    pthread_cond_init(&p->cb_done, NULL);

    // Take the real format if it is there: the feeder's pacing depends on it, and
    // samplesPerSec is milliHz, so a raw copy would be 1000x wrong.
    SLDataSource *s = src;
    if (s && s->pFormat) {
        SLDataFormat_PCM *f = s->pFormat;
        if (f->formatType == SL_DATAFORMAT_PCM) {
            p->rate = f->samplesPerSec / 1000;
            p->channels = f->numChannels;
            p->bits = f->bitsPerSample;
        }
    }
    fprintf(stderr, "  [sl] audio player: %u Hz, %u ch, %u bit\n",
            p->rate, p->channels, p->bits);
    if (out) *out = (SLObjectItf)p;
    return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static const struct SLEngineItf_ g_engine_vt = {
    (void *)eng_unsupported,        // CreateLEDDevice
    (void *)eng_unsupported,        // CreateVibraDevice
    eng_CreateAudioPlayer,
    (void *)eng_unsupported,        // CreateAudioRecorder — no microphone path
    (void *)eng_unsupported,        // CreateMidiPlayer
    (void *)eng_unsupported,        // CreateListener
    (void *)eng_unsupported,        // Create3DGroup
    eng_CreateOutputMix,
    (void *)eng_unsupported,        // CreateMetadataExtractor
    (void *)eng_unsupported,        // CreateExtensionObject
    (void *)eng_unsupported,        // QueryNumSupportedInterfaces
    (void *)eng_unsupported,        // QuerySupportedInterfaces
    (void *)eng_unsupported,        // QueryNumSupportedExtensions
    (void *)eng_unsupported,        // QuerySupportedExtension
    (void *)eng_unsupported,        // IsExtensionSupported
};

// GetInterface hands back a pointer to the right vtable *slot inside the object*,
// because every method takes that pointer back as self.
static SLresult obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *out) {
    if (!out) return SL_RESULT_FEATURE_UNSUPPORTED;
    void *o = (void *)self;
    if (o == (void *)&g_engine && iid == SL_IID_ENGINE) {
        g_engine.engine_vt = &g_engine_vt;
        *(void **)out = &g_engine.engine_vt;
        return SL_RESULT_SUCCESS;
    }
    if (o == (void *)&g_player) {
        if (iid == SL_IID_PLAY) { *(void **)out = &g_player.play_vt; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE || iid == SL_IID_BUFFERQUEUE) {
            *(void **)out = &g_player.bq_vt; return SL_RESULT_SUCCESS;
        }
        if (iid == SL_IID_ANDROIDCONFIGURATION) {
            *(void **)out = &g_player.cfg_vt; return SL_RESULT_SUCCESS;
        }
    }
    fprintf(stderr, "  [sl] GetInterface: unsupported interface %p on %p\n",
            (const void *)iid, o);
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult kl_slCreateEngine(SLObjectItf *out, SLuint32 nopt, void *opts,
                                  SLuint32 nif, const SLInterfaceID *ids,
                                  const SLboolean *req) {
    (void)nopt; (void)opts; (void)nif; (void)ids; (void)req;
    g_engine.obj_vt = &g_obj_vt;
    g_engine.engine_vt = &g_engine_vt;
    if (out) *out = (SLObjectItf)&g_engine;
    fprintf(stderr, "  [sl] slCreateEngine\n");
    return SL_RESULT_SUCCESS;
}

// ---- the library ----
#define KL_SL_MAX 64
static struct { const char *name; unsigned calls; } g_sl[KL_SL_MAX];
static unsigned g_nsl;

static int sl_slot(const char *name) {
    for (unsigned i = 0; i < g_nsl; i++)
        if (strcmp(g_sl[i].name, name) == 0) return (int)i;
    if (g_nsl >= KL_SL_MAX) return -1;
    g_sl[g_nsl].name = strdup(name);
    g_sl[g_nsl].calls = 0;
    return (int)g_nsl++;
}

static uint64_t klsl_called(const char *name) {
    int s = sl_slot(name);
    if (s >= 0) g_sl[s].calls++;
    fprintf(stderr, "\n[klepton] fatal: guest called unimplemented OpenSL ES entry "
                    "point '%s'\n", name);
    kl_fatal_prepare();
    abort();
}

static const char g_sl_handle[] = "klepton-opensles";

int kl_opensl_claims(const char *soname) {
    if (!soname) return 0;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    return strcmp(b, "libOpenSLES.so") == 0;
}

void *kl_opensl_dlopen(const char *soname) {
    if (!kl_opensl_claims(soname)) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    fprintf(stderr, "  [sl] guest dlopen(\"%s\") -> synthetic OpenSL ES handle\n", b);
    return (void *)g_sl_handle;
}

int kl_opensl_is_handle(const void *h) { return h == (const void *)g_sl_handle; }

// The SL_IID_* symbols are *variables* holding interface pointers, so what dlsym
// must return is the address of the variable, not the value. Getting that one
// level of indirection wrong hands FMOD an id it can never match.
static const struct { const char *name; void *addr; } g_sl_syms[] = {
    {"slCreateEngine",                  (void *)kl_slCreateEngine},
    {"SL_IID_ENGINE",                   &SL_IID_ENGINE},
    {"SL_IID_PLAY",                     &SL_IID_PLAY},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &SL_IID_ANDROIDSIMPLEBUFFERQUEUE},
    {"SL_IID_ANDROIDCONFIGURATION",     &SL_IID_ANDROIDCONFIGURATION},
    {"SL_IID_RECORD",                   &SL_IID_RECORD},
    {"SL_IID_BUFFERQUEUE",              &SL_IID_BUFFERQUEUE},
    {"SL_IID_VOLUME",                   &SL_IID_VOLUME},
};

void *kl_opensl_sym(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_sl_syms / sizeof g_sl_syms[0]; i++)
        if (strcmp(g_sl_syms[i].name, name) == 0) return g_sl_syms[i].addr;
    sl_slot(name);
    return kl_named_stub(name, (void *)klsl_called);
}

void kl_opensl_report(FILE *f) {
    static int done;
    if (done) return;
    done = 1;
    if (!g_player.started && !g_nsl) return;
    fprintf(f, "\n=== OpenSL ES ===\n");
    fprintf(f, "  buffers consumed: %lu  (%u Hz, %u ch, %u bit)\n",
            g_buffers_consumed, g_player.rate, g_player.channels, g_player.bits);
    for (unsigned i = 0; i < g_nsl; i++)
        fprintf(f, "    unimplemented: %-32s x%u\n", g_sl[i].name, g_sl[i].calls);
}
