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
// This file used to end with "what this does NOT do is make a sound": the
// feeder timed each buffer with usleep and dropped it, which was enough to keep
// FMOD's mixer running. The PCM now goes to kl_audio.c, which owns a CoreAudio
// output unit, and the *device* provides the clock — kl_audio_write blocks for
// about as long as the audio it accepted takes to play. If no device can be
// opened (or KL_AUDIO=0) the old usleep pacing is still here underneath, so the
// null-audio behaviour every earlier measurement was taken against remains one
// environment variable away.
#include <math.h>
#include <pthread.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread/qos.h>
#include "klepton.h"
#include "kl_env.h"
#include "kl_audio.h"
#include "kl_opensl.h"

typedef uint32_t SLuint32;
typedef int32_t  SLresult;
typedef uint32_t SLboolean;

#define SL_RESULT_SUCCESS            0
#define SL_RESULT_PARAMETER_INVALID  13
#define SL_RESULT_BUFFER_INSUFFICIENT 7
#define SL_RESULT_FEATURE_UNSUPPORTED 12
#define SL_BOOLEAN_FALSE             0
#define SL_OBJECT_STATE_REALIZED     2
#define SL_PLAYSTATE_STOPPED         1
#define SL_PLAYSTATE_PAUSED          2
#define SL_PLAYSTATE_PLAYING         3
// The record-state enum is a SEPARATE numbering from playstate: STOPPED is 0
// here, not 1. A recorder handed a playstate value would sit in the wrong state.
#define SL_RECORDSTATE_STOPPED       0
#define SL_RECORDSTATE_PAUSED        1
#define SL_RECORDSTATE_RECORDING     2

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

// SLRecordItf, in spec order. Only SetRecordState/GetRecordState carry meaning
// here; the rest are the position/marker knobs a minimal capture never needs, so
// they answer success with a zero the same way the play vtable's do.
struct SLRecordItf_;
typedef const struct SLRecordItf_ *const *SLRecordItf;
struct SLRecordItf_ {
    SLresult (*SetRecordState)(SLRecordItf, SLuint32);
    SLresult (*GetRecordState)(SLRecordItf, SLuint32 *);
    SLresult (*SetDurationLimit)(SLRecordItf, SLuint32);
    SLresult (*GetPosition)(SLRecordItf, SLuint32 *);
    SLresult (*RegisterCallback)(SLRecordItf, void *, void *);
    SLresult (*SetCallbackEventsMask)(SLRecordItf, SLuint32);
    SLresult (*GetCallbackEventsMask)(SLRecordItf, SLuint32 *);
    SLresult (*SetMarkerPosition)(SLRecordItf, SLuint32);
    SLresult (*ClearMarkerPosition)(SLRecordItf);
    SLresult (*GetMarkerPosition)(SLRecordItf, SLuint32 *);
    SLresult (*SetPositionUpdatePeriod)(SLRecordItf, SLuint32);
    SLresult (*GetPositionUpdatePeriod)(SLRecordItf, SLuint32 *);
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

// SLAndroidDataFormat_PCM_EX — SLDataFormat_PCM with one field appended, and
// the reason Batman: Arkham Shadow's first audible run was half-speed static.
//
// Oboe opens OpenSL with PCM_EX (formatType 0x4) whenever the requested format
// is Float, which is what Wwise on Meta's EPS spatialiser asks for. A host that
// only recognises formatType 2 falls through to whatever defaults it seeded —
// for us 48000 Hz / 2 ch / 16 bit, which is precisely what an ordinary int16
// player looks like, so the log line agreed with itself and lied. The
// `representation` field is the ONLY thing that separates the two; read it.
typedef struct {
    SLuint32 formatType, numChannels, samplesPerSec, bitsPerSample,
             containerSize, channelMask, endianness, representation;
} SLAndroidDataFormat_PCM_EX;
typedef struct { void *pLocator; void *pFormat; } SLDataSource;
typedef struct { void *pLocator; void *pFormat; } SLDataSink;   // same shape

#define SL_DATAFORMAT_PCM 2
#define SL_ANDROID_DATAFORMAT_PCM_EX 0x4
#define SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT   0x1
#define SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT 0x2
#define SL_ANDROID_PCM_REPRESENTATION_FLOAT        0x3

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
    kl_audio_fmt fmt;                       // ...and its sample encoding
    unsigned long consumed;
} kl_sl_player;

typedef struct { const struct SLObjectItf_ *obj_vt; } kl_sl_mix;
typedef struct {
    const struct SLObjectItf_ *obj_vt;
    const struct SLEngineItf_ *engine_vt;
} kl_sl_engine;

// The capture object — the mirror of kl_sl_player, driving kl_audio.c's mic path
// instead of its output. Best-effort and gated on kl_audio_mic_enabled(): the
// buffer queue holds EMPTY buffers the guest enqueues to be FILLED (the reverse
// of playback), and a feeder pulls the mic, resamples/maps into each buffer, and
// fires the callback so the guest reads it and re-enqueues. 16-bit PCM sinks only
// — the one shape a voice recorder ever asks for — and untested against a real
// guest, since every guest measured here (Steam Link included) records over
// AAudio, which is the fully-exercised path.
typedef struct kl_sl_recorder {
    const struct SLObjectItf_ *obj_vt;      // must stay first: this IS the handle
    const struct SLRecordItf_ *rec_vt;
    const struct SLBufferQueueItf_ *bq_vt;
    const struct SLConfigItf_ *cfg_vt;

    pthread_mutex_t lock;
    pthread_cond_t  wake;
    pthread_cond_t  cb_done;
    pthread_t       thread;
    int             running, started, in_cb;
    SLuint32        state;                   // SL_RECORDSTATE_*

    void (*cb)(SLBufferQueueItf, void *);
    void *cb_ctx;

    struct { void *buf; SLuint32 size; } q[KL_SL_QUEUE];   // buffers to FILL
    unsigned head, tail, count;
    unsigned generation;

    unsigned rate, channels, bits;           // the guest sink format
    unsigned mic_rate, mic_ch;               // the capture format we resample FROM
    int16_t *in_hold;                        // mic frames pulled, not yet consumed
    size_t   in_hold_cap, in_have;
    double   in_pos;
    unsigned long filled;
} kl_sl_recorder;

static kl_sl_engine g_engine;
static kl_sl_mix    g_mix;
static kl_sl_recorder g_recorder;
// Multiple concurrent players. A guest can bring up more than one OpenSL output
// at once — vampire does: libfmod opens its 48 kHz mix AND libUE4 opens its own
// 44.1 kHz output. A single g_player made the second CreateAudioPlayer memset the
// first out of existence (player_stop + memset), so FMOD's live, playing output
// was destroyed the instant UE4's was created and the game went silent. Each slot
// is an independent player with its own feeder; kl_audio mixes them by source key
// (kl_audio_write_src, the same per-source path Steam Link's two streams use).
#define KL_SL_MAX_PLAYERS 4
static kl_sl_player g_players[KL_SL_MAX_PLAYERS];
static pthread_mutex_t g_players_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_buffers_consumed;
static unsigned long g_enqueued;        // Enqueue() calls the guest made
static unsigned long g_enqueue_full;    // ...and the ones we refused, queue full
static unsigned long g_callbacks;       // buffer-queue callbacks we delivered

// A heartbeat, not a report. Everything below is already counted; none of it
// has ever been printed on device, because the end-of-run report only runs on a
// clean exit of the synthetic lifecycle and a headset build is killed or
// backgrounded instead. It is driven from the app's own [hb] ticker rather than
// from the feeder thread on purpose: "the player was never created" is one of
// the answers we need, and a heartbeat that lives inside the player cannot give
// it.
void kl_opensl_heartbeat(FILE *f) {
    // The first live player speaks for the set; the counters are global.
    const kl_sl_player *p = NULL;
    int nstarted = 0;
    for (int i = 0; i < KL_SL_MAX_PLAYERS; i++)
        if (g_players[i].started) { nstarted++; if (!p) p = &g_players[i]; }
    if (!p) {
        fprintf(f, "  [sl] heartbeat: no audio player has ever been created "
                   "(the guest never asked OpenSL for an output)\n");
        return;
    }
    fprintf(f, "  [sl] heartbeat: %d player(s), first state=%s queued=%u enqueued=%lu (full %lu) "
               "consumed=%lu callbacks=%lu  (%u Hz, %u ch, %u bit, %s)\n",
            nstarted, p->state == SL_PLAYSTATE_PLAYING ? "PLAYING"
          : p->state == SL_PLAYSTATE_PAUSED  ? "PAUSED"  : "STOPPED",
            p->count, g_enqueued, g_enqueue_full, g_buffers_consumed, g_callbacks,
            p->rate, p->channels, p->bits, kl_audio_fmt_name(p->fmt));
    kl_audio_heartbeat(f);
}

// Resolve the owning player from an interface pointer. The object interface
// pointer IS the player (obj_vt is the first field); the play/bq/cfg pointers sit
// at fixed offsets inside it. obj_as_player returns NULL for the engine/mix
// objects, which share the object vtable but are not players.
static kl_sl_player *obj_as_player(const void *self) {
    for (int i = 0; i < KL_SL_MAX_PLAYERS; i++)
        if ((const void *)&g_players[i] == self) return &g_players[i];
    return NULL;
}
#define KL_SL_PLAY_P(self) ((kl_sl_player *)((char *)(self) - offsetof(kl_sl_player, play_vt)))
#define KL_SL_BQ_P(self)   ((kl_sl_player *)((char *)(self) - offsetof(kl_sl_player, bq_vt)))
#define KL_SL_CFG_P(self)  ((kl_sl_player *)((char *)(self) - offsetof(kl_sl_player, cfg_vt)))

// The recorder's own container-of macros. Its bq/cfg vtable pointers sit at
// different offsets than the player's, so they need their own recovery — a
// shared buffer-queue vtable would compute the wrong object for whichever type
// it was not written for.
#define KL_SL_REC_R(self)    ((kl_sl_recorder *)((char *)(self) - offsetof(kl_sl_recorder, rec_vt)))
#define KL_SL_REC_BQ_R(self) ((kl_sl_recorder *)((char *)(self) - offsetof(kl_sl_recorder, bq_vt)))

static kl_sl_recorder *obj_as_recorder(const void *self) {
    return (const void *)&g_recorder == self ? &g_recorder : NULL;
}

static void player_stop(kl_sl_player *p);
static void player_wait_cb(kl_sl_player *p);   // caller must hold p->lock
static void recorder_stop(kl_sl_recorder *r);
static void *rec_feeder(void *arg);

// ---- the feeder ----
//
// FMOD enqueues a buffer, we "play" it for as long as it would really take, then
// call back so it enqueues the next. That callback runs guest code on this
// thread, so kl_thread_init() is mandatory before the first one — without
// it the stack-protector prologue reads an empty TSD slot and the guest dies far
// from here.
static void *feeder(void *arg) {
    // Audio feeder at HIGH QoS. At default QoS this thread is throttled the
    // moment the compositor and guest saturate the P-cores, and every
    // scheduling gap longer than one burst is an audible skip (Steam Link) or
    // crackle (Wwise starvation). USER_INTERACTIVE is the class visionOS
    // schedules ahead of default work; the knob returns the old behaviour.
#ifdef __APPLE__
    if (kl_env_on("KL_AUDIO_QOS", 1))
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    kl_sl_player *p = arg;
    kl_thread_init();
    // Same argument as kl_aaudio.c's feeder: this thread calls the guest's
    // mixer and then blocks for one buffer's playback, so a preemption lands
    // in the output as a hole. A plain pthread_attr is QOS_CLASS_UNSPECIFIED,
    // which on Apple silicon is the tier the scheduler may satisfy from an
    // efficiency core.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
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
        const void *buf = p->q[p->head].buf;
        SLuint32 size = p->q[p->head].size;
        p->head = (p->head + 1) % KL_SL_QUEUE;
        p->count--;
        unsigned frame = p->channels * kl_audio_fmt_bytes(p->fmt);
        if (!frame) frame = 4;
        unsigned rate = p->rate ? p->rate : 48000;
        unsigned generation = p->generation;
        pthread_mutex_unlock(&p->lock);

        // Hand the PCM to the device, and let it be the clock. The buffer is
        // FMOD's and it may reuse it the instant the callback below returns, so
        // kl_audio_write copies rather than referencing — and it must therefore
        // happen before the callback, not after.
        //
        // A short write (no device, KL_AUDIO=0, or a device that stopped
        // draining) leaves the remainder to be paced the old way. That keeps one
        // code path for both cases: with no audio, `played` is 0 and this is
        // byte-for-byte the usleep loop it replaced.
        size_t played = buf ? kl_audio_write_src(p, buf, size) : 0;
        if (played < size) {
            uint64_t rem = ((uint64_t)size - played) / frame;
            useconds_t us = (useconds_t)(rem * 1000000ull / rate);
            if (us) usleep(us > 100000 ? 100000 : us);   // cap, so a bogus size cannot wedge us
        }

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
        if (cb) { g_callbacks++; cb((SLBufferQueueItf)&p->bq_vt, cb_ctx); }

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
    kl_sl_player *p = obj_as_player(self);
    if (p && !p->started) {
        // Open this player's SOURCE before the feeder exists, so its first buffer
        // already has somewhere to go. Every player is its own source to the sink,
        // at its own rate and encoding (a UE4 title can run UE4's own 44.1 kHz
        // output alongside FMOD's 48 kHz) — kl_audio.h says which one owns the output and how the
        // others are paced (KL_AUDIO_MIX_SOURCES=1 mixes them instead). A failure
        // here is not fatal: the feeder falls back to usleep pacing, which is what
        // this player did for its whole life before kl_audio.c.
        kl_audio_open_src_fmt(p, "opensl", p->rate, p->channels, p->fmt);
        p->started = p->running = 1;
        pthread_create(&p->thread, NULL, feeder, p);
    }
    // The recorder realizes the same way: the mic was opened at CreateAudioRecorder
    // (so the resample params are known); here the feeder starts pulling it.
    kl_sl_recorder *r = obj_as_recorder(self);
    if (r && !r->started) {
        r->started = r->running = 1;
        pthread_create(&r->thread, NULL, rec_feeder, r);
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
    kl_sl_player *p = obj_as_player(self);
    if (p) { player_stop(p); return; }
    kl_sl_recorder *r = obj_as_recorder(self);
    if (r) recorder_stop(r);
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
    kl_sl_player *p = KL_SL_PLAY_P(self);
    // Once per distinct state. "Did the guest ever start the stream?" is one
    // bit, and without this line no device log can answer it.
    static SLuint32 said = (SLuint32)-1;
    if (state != said) {
        said = state;
        fprintf(stderr, "  [sl] SetPlayState(%s)\n",
                state == SL_PLAYSTATE_PLAYING ? "PLAYING"
              : state == SL_PLAYSTATE_PAUSED  ? "PAUSED" : "STOPPED");
    }
    // Before the lock, and before waiting: the feeder may be parked inside
    // kl_audio_write waiting for the ring to drain, and that wait only ends
    // when the sink stops being "playing". Doing this after player_wait_cb
    // would be waiting for a thread we have not yet told to stop. Per player:
    // each is its own source, so pausing one never silences another.
    if (state != SL_PLAYSTATE_PLAYING) kl_audio_pause_src(p);
    pthread_mutex_lock(&p->lock);
    p->state = state;
    pthread_cond_signal(&p->wake);
    // Stopping is synchronous: do not return until the guest's callback is off
    // the stack, or it will run against buffers the guest is about to release.
    if (state != SL_PLAYSTATE_PLAYING) player_wait_cb(p);
    pthread_mutex_unlock(&p->lock);
    // The queued audio is dropped rather than drained, for the same reason the
    // callback wait above exists: the guest stops precisely when it is about to
    // release the buffers it enqueued, and real OpenSL does not go on playing
    // them. Draining would sound better and be wrong.
    if (state == SL_PLAYSTATE_PLAYING) kl_audio_play_src(p);
    else                               kl_audio_flush_src(p);
    fprintf(stderr, "  [sl] play state -> %s\n",
            state == SL_PLAYSTATE_PLAYING ? "PLAYING" :
            state == SL_PLAYSTATE_PAUSED  ? "PAUSED" : "STOPPED");
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(SLPlayItf s, SLuint32 *st) {
    if (st) *st = KL_SL_PLAY_P(s)->state; return SL_RESULT_SUCCESS;
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
    kl_sl_player *p = KL_SL_BQ_P(self);
    pthread_mutex_lock(&p->lock);
    if (p->count == KL_SL_QUEUE) {
        g_enqueue_full++;
        pthread_mutex_unlock(&p->lock);
        return SL_RESULT_FEATURE_UNSUPPORTED;   // queue full; FMOD retries
    }
    g_enqueued++;
    p->q[p->tail].buf = buf;
    p->q[p->tail].size = size;
    p->tail = (p->tail + 1) % KL_SL_QUEUE;
    p->count++;
    pthread_cond_signal(&p->wake);
    pthread_mutex_unlock(&p->lock);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_Clear(SLBufferQueueItf self) {
    kl_sl_player *p = KL_SL_BQ_P(self);
    pthread_mutex_lock(&p->lock);
    p->head = p->tail = p->count = 0;
    pthread_mutex_unlock(&p->lock);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(SLBufferQueueItf self, SLBufferQueueState *st) {
    kl_sl_player *p = KL_SL_BQ_P(self);
    if (st) { st->count = p->count; st->index = (SLuint32)p->consumed; }
    return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(SLBufferQueueItf self,
                                    void (*cb)(SLBufferQueueItf, void *), void *ctx) {
    kl_sl_player *p = KL_SL_BQ_P(self);
    p->cb = cb;
    p->cb_ctx = ctx;
    return SL_RESULT_SUCCESS;
}
static const struct SLBufferQueueItf_ g_bq_vt = {
    bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback,
};

// ---- SLAndroidConfigurationItf ----
//
// "Recorded, not applied" was only ever half true: nothing was recorded, and
// GetConfiguration refused every key with SL_RESULT_FEATURE_UNSUPPORTED. That
// refusal is what silenced Batman.
//
// On Android these keys always HAVE a value — the ones the guest never set have
// the platform default — so `unsupported` is not the conservative answer here,
// it is a claim the interface exists and holds nothing, which is not a state
// real OpenSL can be in. And a caller is entitled to treat it as fatal: the
// oboe 1.7.0 built into libMetaXRAudioWwise.so does exactly that. Its
// finishCommonOpen reads SL_ANDROID_KEY_PERFORMANCE_MODE, and on a nonzero SL
// result it returns that result rather than shrugging (upstream oboe only
// warns), so open() gives ErrorInternal, the Meta endpoint sink's device
// creation remaps it to eps code 25 — printed as the maddeningly unrelated
// "Unsupported format" — and Wwise ends the run with no sink at all.
//
// So: record what the guest sets, hand it back, and answer an unset key with
// the documented default rather than a refusal. All the android* keys are
// 32-bit.
#define KL_SL_CFG_MAX 8
static struct { char key[40]; uint32_t val; } g_cfg[KL_SL_CFG_MAX];
static unsigned g_ncfg;

static SLresult cfg_Set(SLConfigItf s, const char *k, const void *v, SLuint32 n) {
    (void)s;
    if (!k) return SL_RESULT_PARAMETER_INVALID;
    uint32_t val = (v && n >= 4) ? *(const uint32_t *)v : 0;
    unsigned i = 0;
    for (; i < g_ncfg; i++) if (strcmp(g_cfg[i].key, k) == 0) break;
    if (i == g_ncfg && g_ncfg < KL_SL_CFG_MAX) {
        snprintf(g_cfg[g_ncfg].key, sizeof g_cfg[0].key, "%s", k);
        g_ncfg++;
    }
    if (i < KL_SL_CFG_MAX) g_cfg[i].val = val;
    fprintf(stderr, "  [sl] SetConfiguration(\"%s\") = %u — recorded, and readable back\n",
            k, val);
    return SL_RESULT_SUCCESS;
}

static SLresult cfg_Get(SLConfigItf s, const char *k, SLuint32 *n, void *v) {
    (void)s;
    if (!k || !n) return SL_RESULT_PARAMETER_INVALID;
    uint32_t val = 0;                       // the platform default for every
                                            // android* key we serve: performance
                                            // mode NONE, stream type MEDIA(3) is
                                            // the one exception and nobody reads it
    int known = 0;
    for (unsigned i = 0; i < g_ncfg; i++)
        if (strcmp(g_cfg[i].key, k) == 0) { val = g_cfg[i].val; known = 1; break; }
    // pValue NULL is the size query.
    if (!v) { *n = 4; return SL_RESULT_SUCCESS; }
    if (*n < 4) { *n = 4; return SL_RESULT_BUFFER_INSUFFICIENT; }
    *(uint32_t *)v = val;
    *n = 4;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [sl] GetConfiguration(\"%s\") -> %u (%s). A refusal here is "
                        "fatal to oboe's OpenSL open, which is how a live stream "
                        "became \"Unsupported format\" three layers up.\n",
                k, val, known ? "as the guest set it" : "platform default, never set");
    }
    return SL_RESULT_SUCCESS;
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
    kl_audio_pause_src(p);     // so a feeder parked in kl_audio_write can be joined
    pthread_mutex_lock(&p->lock);
    p->running = 0;
    p->state = SL_PLAYSTATE_STOPPED;
    p->generation++;
    pthread_cond_signal(&p->wake);
    pthread_mutex_unlock(&p->lock);
    pthread_join(p->thread, NULL);
    p->started = 0;
    kl_audio_close_src(p);     // only after the join: the feeder is the producer
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
// hang rather than crash — which is the harder failure to read.
static void player_wait_cb(kl_sl_player *p) {
    if (p->started && !pthread_equal(pthread_self(), p->thread))
        while (p->in_cb) pthread_cond_wait(&p->cb_done, &p->lock);
}

static SLresult eng_CreateAudioPlayer(SLEngineItf self, SLObjectItf *out,
                                      void *src, void *snk, SLuint32 n,
                                      const SLInterfaceID *ids, const SLboolean *req) {
    (void)self; (void)snk; (void)n; (void)ids; (void)req;
    // A FREE slot, so a second output (vampire: UE4 alongside FMOD) does not stop
    // and memset a live player out from under its feeder. Only when all slots are
    // in use do we fall back to reclaiming slot 0.
    pthread_mutex_lock(&g_players_lock);
    kl_sl_player *p = NULL;
    for (int i = 0; i < KL_SL_MAX_PLAYERS; i++)
        if (!g_players[i].started) { p = &g_players[i]; break; }
    if (!p) p = &g_players[0];
    pthread_mutex_unlock(&g_players_lock);
    player_stop(p);                 // no-op for a free slot (!started)
    unsigned gen = p->generation;
    memset(p, 0, sizeof *p);
    p->generation = gen;
    p->obj_vt = &g_obj_vt;
    p->play_vt = &g_play_vt;
    p->bq_vt = &g_bq_vt;
    p->cfg_vt = &g_cfg_vt;
    p->state = SL_PLAYSTATE_STOPPED;
    p->rate = 48000; p->channels = 2; p->bits = 16; p->fmt = KL_AUDIO_S16;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->wake, NULL);
    pthread_cond_init(&p->cb_done, NULL);

    // Take the real format if it is there: the feeder's pacing depends on it, and
    // samplesPerSec is milliHz, so a raw copy would be 1000x wrong.
    SLDataSource *s = src;
    if (s && s->pFormat) {
        SLDataFormat_PCM *f = s->pFormat;
        if (f->formatType == SL_DATAFORMAT_PCM ||
            f->formatType == SL_ANDROID_DATAFORMAT_PCM_EX) {
            p->rate = f->samplesPerSec / 1000;
            p->channels = f->numChannels;
            p->bits = f->bitsPerSample;
            // Plain PCM has no representation field and means signed integer at
            // every width but 8; PCM_EX carries one, and reading past the end of
            // a plain-PCM struct to find it is how this would go wrong the other
            // way. So the width mapping is shared and only PCM_EX overrides it.
            unsigned rep = f->formatType == SL_ANDROID_DATAFORMAT_PCM_EX
                         ? ((SLAndroidDataFormat_PCM_EX *)f)->representation
                         : SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT;
            switch (p->bits) {
            case 8:  p->fmt = KL_AUDIO_U8;  break;
            case 24: p->fmt = KL_AUDIO_S24; break;
            case 32: p->fmt = KL_AUDIO_S32; break;
            default: p->fmt = KL_AUDIO_S16; break;
            }
            if (rep == SL_ANDROID_PCM_REPRESENTATION_FLOAT) {
                if (p->bits == 32) {
                    p->fmt = KL_AUDIO_F32;
                } else {
                    // A float representation at any other width is not something
                    // Android defines. Say so rather than reading it as ints.
                    fprintf(stderr, "  [sl] PCM_EX claims float at %u bit, which "
                                    "is not a format — treating it as 32-bit float\n",
                            p->bits);
                    p->bits = 32;
                    p->fmt = KL_AUDIO_F32;
                }
            } else if (rep == SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT && p->bits != 8) {
                fprintf(stderr, "  [sl] PCM_EX asks for unsigned %u-bit, which we "
                                "only implement at 8 — playing it as signed\n", p->bits);
            }
        } else {
            fprintf(stderr, "  [sl] audio source format type 0x%x is neither PCM "
                            "nor PCM_EX — keeping the 48000/2/s16 default\n",
                    f->formatType);
        }
    }
    // The format is on this line because 48000/2/16 is
    // both the truth for FMOD and the default we fall back to for everyone else,
    // so a line without the encoding cannot tell a parsed format from a guess.
    fprintf(stderr, "  [sl] audio player: %u Hz, %u ch, %u bit, %s\n",
            p->rate, p->channels, p->bits, kl_audio_fmt_name(p->fmt));
    if (out) *out = (SLObjectItf)p;
    return SL_RESULT_SUCCESS;
}

// ---- the recorder (capture) ----
//
// Produce up to `frames` guest frames (int16, r->channels interleaved) into dst
// from the mic, resampling r->mic_rate -> r->rate and mapping channels. One
// non-blocking pass — it pulls whatever the mic ring holds and converts it; the
// feeder loops with a short sleep to fill a whole buffer. mic_rate == r->rate
// (the usual case) makes the ratio 1.0 and this a straight copy with channel map.
static int rec_pull(kl_sl_recorder *r, int16_t *dst, int frames) {
    if (frames <= 0) return 0;
    unsigned mch = r->mic_ch ? r->mic_ch : 1;
    unsigned gch = r->channels ? r->channels : 1;
    double ratio = (double)(r->mic_rate ? r->mic_rate : r->rate)
                 / (double)(r->rate ? r->rate : 48000);
    size_t need = (size_t)((double)frames * ratio) + 2;
    if (need > r->in_hold_cap) {
        int16_t *p = realloc(r->in_hold, need * mch * sizeof(int16_t));
        if (!p) return 0;
        r->in_hold = p; r->in_hold_cap = need;
    }
    if (r->in_have < need) {
        int got = kl_audio_mic_read_i16(r->in_hold + r->in_have * mch,
                                        (int)(need - r->in_have));
        if (got > 0) r->in_have += (size_t)got;
    }
    int produced = 0;
    while (produced < frames) {
        double pos = r->in_pos;
        size_t i = (size_t)pos;
        if (i + 1 >= r->in_have) break;               // not enough mic held yet
        float frac = (float)(pos - (double)i);
        for (unsigned c = 0; c < gch; c++) {
            unsigned sc = c < mch ? c : 0;            // mono fans out; extra -> ch0
            float a = (float)r->in_hold[i * mch + sc];
            float b = (float)r->in_hold[(i + 1) * mch + sc];
            float v = a + (b - a) * frac;
            if (v > 32767.0f) v = 32767.0f; else if (v < -32768.0f) v = -32768.0f;
            dst[(size_t)produced * gch + c] = (int16_t)lrintf(v);
        }
        produced++;
        r->in_pos = pos + ratio;
    }
    size_t base = (size_t)r->in_pos;
    if (base > 0) {
        if (base > r->in_have) base = r->in_have;
        size_t rem = r->in_have - base;
        if (rem) memmove(r->in_hold, r->in_hold + base * mch, rem * mch * sizeof(int16_t));
        r->in_have = rem;
        r->in_pos -= (double)base;
    }
    return produced;
}

// The record feeder — the mirror of the playback feeder. The buffer queue holds
// EMPTY buffers the guest enqueued; this dequeues one, fills it fully from the
// mic (pacing on the mic's own rate), then fires the callback so the guest reads
// the samples and re-enqueues. Guest code runs on this thread, so kl_thread_init()
// is mandatory, and the callback is held across in_cb exactly as playback does so
// a concurrent stop/destroy waits rather than freeing underneath it.
static void *rec_feeder(void *arg) {
#ifdef __APPLE__
    if (kl_env_on("KL_AUDIO_QOS", 1))
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    kl_sl_recorder *r = arg;
    kl_thread_init();
    pthread_mutex_lock(&r->lock);
    while (r->running) {
        if (r->state != SL_RECORDSTATE_RECORDING || r->count == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 5 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&r->wake, &r->lock, &ts);
            continue;
        }
        void *buf = r->q[r->head].buf;
        SLuint32 size = r->q[r->head].size;
        r->head = (r->head + 1) % KL_SL_QUEUE;
        r->count--;
        unsigned gen = r->generation;
        unsigned frame = (r->channels * r->bits) / 8;
        if (!frame) frame = 2;
        int frames = (int)(size / frame);
        pthread_mutex_unlock(&r->lock);

        // Fill the whole buffer, re-checking state each pass so a stop unblocks
        // promptly rather than after a full buffer's worth of mic.
        int done = 0;
        while (done < frames) {
            pthread_mutex_lock(&r->lock);
            int stop = !r->running || gen != r->generation ||
                       r->state != SL_RECORDSTATE_RECORDING;
            pthread_mutex_unlock(&r->lock);
            if (stop) break;
            int got = rec_pull(r, (int16_t *)buf + (size_t)done * r->channels, frames - done);
            done += got;
            if (done < frames) usleep(2000);
        }

        pthread_mutex_lock(&r->lock);
        if (!r->running || gen != r->generation) break;
        if (r->state != SL_RECORDSTATE_RECORDING) continue;
        void (*cb)(SLBufferQueueItf, void *) = r->cb;
        void *cb_ctx = r->cb_ctx;
        r->in_cb = 1;
        pthread_mutex_unlock(&r->lock);

        r->filled++;
        if (cb) cb((SLBufferQueueItf)&r->bq_vt, cb_ctx);

        pthread_mutex_lock(&r->lock);
        r->in_cb = 0;
        pthread_cond_broadcast(&r->cb_done);
        if (gen != r->generation) break;
    }
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

// The recorder's SLAndroidSimpleBufferQueueItf. Same four methods as the player's
// but recovering the recorder container (its bq_vt sits at a different offset), and
// Enqueue takes buffers to be FILLED rather than played.
static SLresult rec_bq_Enqueue(SLBufferQueueItf self, const void *buf, SLuint32 size) {
    kl_sl_recorder *r = KL_SL_REC_BQ_R(self);
    pthread_mutex_lock(&r->lock);
    if (r->count == KL_SL_QUEUE) { pthread_mutex_unlock(&r->lock); return SL_RESULT_FEATURE_UNSUPPORTED; }
    r->q[r->tail].buf = (void *)buf;        // written into, not read from
    r->q[r->tail].size = size;
    r->tail = (r->tail + 1) % KL_SL_QUEUE;
    r->count++;
    pthread_cond_signal(&r->wake);
    pthread_mutex_unlock(&r->lock);
    return SL_RESULT_SUCCESS;
}
static SLresult rec_bq_Clear(SLBufferQueueItf self) {
    kl_sl_recorder *r = KL_SL_REC_BQ_R(self);
    pthread_mutex_lock(&r->lock);
    r->head = r->tail = r->count = 0;
    pthread_mutex_unlock(&r->lock);
    return SL_RESULT_SUCCESS;
}
static SLresult rec_bq_GetState(SLBufferQueueItf self, SLBufferQueueState *st) {
    kl_sl_recorder *r = KL_SL_REC_BQ_R(self);
    if (st) { st->count = r->count; st->index = (SLuint32)r->filled; }
    return SL_RESULT_SUCCESS;
}
static SLresult rec_bq_RegisterCallback(SLBufferQueueItf self,
                                        void (*cb)(SLBufferQueueItf, void *), void *ctx) {
    kl_sl_recorder *r = KL_SL_REC_BQ_R(self);
    r->cb = cb;
    r->cb_ctx = ctx;
    return SL_RESULT_SUCCESS;
}
static const struct SLBufferQueueItf_ g_rec_bq_vt = {
    rec_bq_Enqueue, rec_bq_Clear, rec_bq_GetState, rec_bq_RegisterCallback,
};

// The SLRecordItf. Only the record-state pair does anything; the marker/position
// knobs answer success with a zero, as the play vtable's spare slots do.
static SLresult rec_SetRecordState(SLRecordItf self, SLuint32 state) {
    kl_sl_recorder *r = KL_SL_REC_R(self);
    pthread_mutex_lock(&r->lock);
    r->state = state;
    pthread_cond_signal(&r->wake);
    // Stopping/pausing is synchronous with respect to the callback, so the guest
    // may free what it touches the moment this returns (and never wait on
    // ourselves if called from inside the callback).
    if (state != SL_RECORDSTATE_RECORDING && r->started &&
        !pthread_equal(pthread_self(), r->thread))
        while (r->in_cb) pthread_cond_wait(&r->cb_done, &r->lock);
    pthread_mutex_unlock(&r->lock);
    fprintf(stderr, "  [sl] record state -> %s\n",
            state == SL_RECORDSTATE_RECORDING ? "RECORDING" :
            state == SL_RECORDSTATE_PAUSED    ? "PAUSED" : "STOPPED");
    return SL_RESULT_SUCCESS;
}
static SLresult rec_GetRecordState(SLRecordItf self, SLuint32 *st) {
    if (st) *st = KL_SL_REC_R(self)->state;
    return SL_RESULT_SUCCESS;
}
static SLresult rec_SetU32(SLRecordItf s, SLuint32 v)  { (void)s; (void)v; return SL_RESULT_SUCCESS; }
static SLresult rec_GetU32(SLRecordItf s, SLuint32 *v) { (void)s; if (v) *v = 0; return SL_RESULT_SUCCESS; }
static SLresult rec_Reg(SLRecordItf s, void *c, void *x) { (void)s; (void)c; (void)x; return SL_RESULT_SUCCESS; }
static SLresult rec_Clear(SLRecordItf s) { (void)s; return SL_RESULT_SUCCESS; }
static const struct SLRecordItf_ g_rec_vt = {
    rec_SetRecordState, rec_GetRecordState, rec_SetU32 /*SetDurationLimit*/,
    rec_GetU32 /*GetPosition*/, rec_Reg /*RegisterCallback*/,
    rec_SetU32 /*SetCallbackEventsMask*/, rec_GetU32 /*GetCallbackEventsMask*/,
    rec_SetU32 /*SetMarkerPosition*/, rec_Clear /*ClearMarkerPosition*/,
    rec_GetU32 /*GetMarkerPosition*/, rec_SetU32 /*SetPositionUpdatePeriod*/,
    rec_GetU32 /*GetPositionUpdatePeriod*/,
};

// Stop and join the record feeder, then close the mic. Mirrors player_stop; the
// mic device is not shared across recorders (there is one g_recorder), so it is
// closed here unconditionally.
static void recorder_stop(kl_sl_recorder *r) {
    if (!r->started) return;
    pthread_mutex_lock(&r->lock);
    r->running = 0;
    r->state = SL_RECORDSTATE_STOPPED;
    r->generation++;
    pthread_cond_signal(&r->wake);
    pthread_mutex_unlock(&r->lock);
    pthread_join(r->thread, NULL);
    r->started = 0;
    kl_audio_mic_close();
    free(r->in_hold); r->in_hold = NULL; r->in_hold_cap = 0; r->in_have = 0;
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->wake);
    pthread_cond_destroy(&r->cb_done);
}

static SLresult eng_CreateAudioRecorder(SLEngineItf self, SLObjectItf *out,
                                        void *src, void *snk, SLuint32 n,
                                        const SLInterfaceID *ids, const SLboolean *req) {
    (void)self; (void)src; (void)n; (void)ids; (void)req;
    // Gated: with the microphone toggle OFF this is the same refusal it always
    // was, and nothing touches the mic. The AAudio input path is the priority and
    // fully exercised; this OpenSL recorder is best-effort for a guest that
    // records over OpenSL ES instead.
    if (!kl_audio_mic_enabled()) {
        fprintf(stderr, "  [sl] CreateAudioRecorder -> UNSUPPORTED (microphone toggle is OFF)\n");
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }

    kl_sl_recorder *r = &g_recorder;
    recorder_stop(r);                       // no-op if not started
    unsigned gen = r->generation;
    memset(r, 0, sizeof *r);
    r->generation = gen;
    r->obj_vt = &g_obj_vt;
    r->rec_vt = &g_rec_vt;
    r->bq_vt  = &g_rec_bq_vt;
    r->cfg_vt = &g_cfg_vt;
    r->state = SL_RECORDSTATE_STOPPED;
    r->rate = 48000; r->channels = 1; r->bits = 16;

    // The sink PCM format (samplesPerSec is milliHz, as on the player side).
    SLDataSink *sink = snk;
    if (sink && sink->pFormat) {
        SLDataFormat_PCM *f = sink->pFormat;
        if (f->formatType == SL_DATAFORMAT_PCM) {
            r->rate = f->samplesPerSec / 1000;
            r->channels = f->numChannels;
            r->bits = f->bitsPerSample;
        }
    }
    if (r->bits != 16) {
        fprintf(stderr, "  [sl] CreateAudioRecorder: %u-bit sink not implemented "
                        "(16-bit PCM only) -> UNSUPPORTED\n", r->bits);
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }

    if (kl_audio_mic_open(r->rate, r->channels) != 0) {
        fprintf(stderr, "  [sl] CreateAudioRecorder: mic toggle on but "
                        "kl_audio_mic_open failed -> UNSUPPORTED\n");
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    r->mic_rate = kl_audio_mic_rate();
    r->mic_ch   = kl_audio_mic_channels();

    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->wake, NULL);
    pthread_cond_init(&r->cb_done, NULL);
    fprintf(stderr, "  [sl] audio recorder: %u Hz, %u ch, %u bit <- mic %u Hz/%u ch\n",
            r->rate, r->channels, r->bits, r->mic_rate, r->mic_ch);
    if (out) *out = (SLObjectItf)r;
    return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static const struct SLEngineItf_ g_engine_vt = {
    (void *)eng_unsupported,        // CreateLEDDevice
    (void *)eng_unsupported,        // CreateVibraDevice
    eng_CreateAudioPlayer,
    eng_CreateAudioRecorder,        // opt-in mic path, gated on the KleptonMic toggle
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
    kl_sl_player *p = obj_as_player(o);
    if (p) {
        if (iid == SL_IID_PLAY) { *(void **)out = &p->play_vt; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE || iid == SL_IID_BUFFERQUEUE) {
            *(void **)out = &p->bq_vt; return SL_RESULT_SUCCESS;
        }
        if (iid == SL_IID_ANDROIDCONFIGURATION) {
            *(void **)out = &p->cfg_vt; return SL_RESULT_SUCCESS;
        }
    }
    kl_sl_recorder *r = obj_as_recorder(o);
    if (r) {
        if (iid == SL_IID_RECORD) { *(void **)out = &r->rec_vt; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE || iid == SL_IID_BUFFERQUEUE) {
            *(void **)out = &r->bq_vt; return SL_RESULT_SUCCESS;
        }
        if (iid == SL_IID_ANDROIDCONFIGURATION) {
            *(void **)out = &r->cfg_vt; return SL_RESULT_SUCCESS;
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
    kl_sl_player *rep = NULL;
    int nstarted = 0;
    for (int i = 0; i < KL_SL_MAX_PLAYERS; i++)
        if (g_players[i].started) { nstarted++; if (!rep) rep = &g_players[i]; }
    if (!rep && !g_nsl && !g_buffers_consumed) return;
    fprintf(f, "\n=== OpenSL ES ===\n");
    fprintf(f, "  buffers consumed: %lu  (%u player(s); first %u Hz, %u ch, %u bit, %s)\n",
            g_buffers_consumed, nstarted, rep ? rep->rate : 0,
            rep ? rep->channels : 0, rep ? rep->bits : 0,
            rep ? kl_audio_fmt_name(rep->fmt) : "-");
    fprintf(f, "  enqueued: %lu (%lu refused, queue full), callbacks delivered: %lu\n",
            g_enqueued, g_enqueue_full, g_callbacks);
    if (g_recorder.filled)
        fprintf(f, "  recorder: %lu buffer(s) filled (%u Hz, %u ch, %u bit <- mic %u Hz/%u ch)\n",
                g_recorder.filled, g_recorder.rate, g_recorder.channels, g_recorder.bits,
                g_recorder.mic_rate, g_recorder.mic_ch);
    // The buffer count is exactly the assertion that passed for months while
    // nothing played; the line below is the one that says whether it was heard.
    kl_audio_report(f);
    for (unsigned i = 0; i < g_nsl; i++)
        fprintf(f, "    unimplemented: %-32s x%u\n", g_sl[i].name, g_sl[i].calls);
}
