// See kl_mediandk.h. The guest-facing half of the video path; kl_vtdec.c is the
// decoder.
//
// Three objects and the relationships between them:
//
//   AMediaCodec   owns a kl_vtdec, a pool of input buffers, and a set of output
//                 slots holding decoded frames the guest has been told about
//                 but has not yet released.
//   AImageReader  is the codec's output surface. Release-with-render moves a
//                 frame from an output slot into the reader's queue and wakes
//                 the reader's dispatch thread, which calls the guest's
//                 listener.
//   AImage        is one frame taken off that queue, and AHardwareBuffer is a
//                 refcounted handle on the same CVPixelBuffer that outlives it.
//
// The dispatch thread is not an implementation detail — see the note above
// reader_dispatch for why calling the listener inline would deadlock.
#include "kl_mediandk.h"

#include <fcntl.h>          // F_GETPATH, to name the fd a demux was asked for
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_ndk.h"
#include "kl_vtdec.h"

void kl_unresolved_named(const char *name);     // kl_shim.c

// ---------------------------------------------------------------------------
// The ABI's own numbers. Transcribed from <media/NdkMediaError.h>,
// <media/NdkMediaCodec.h> and <media/NdkImage.h> — the guest tests these
// exactly, so they are contract, not preference.
// The whole ladder, spelled out, because it is derived arithmetic in the header
// (`AMEDIA_ERROR_BASE - n`) and INVALID_PARAMETER was transcribed one step short
// — it was -10003, which is INVALID_OBJECT, a DIFFERENT named error in the same
// enum. Checked against the NDK's own <media/NdkMediaError.h>. No guest is known
// to distinguish the two (Steam Link's decoder path tests against AMEDIA_OK),
// but that is an absence of evidence rather than a proof, which is exactly why
// the comment above calls these contract rather than preference: a value that is
// wrong and never load-bearing is the kind that stays wrong until it is.
enum { AMEDIA_OK = 0,
       AMEDIA_ERROR_UNKNOWN           = -10000,
       AMEDIA_ERROR_MALFORMED         = -10001,
       AMEDIA_ERROR_UNSUPPORTED       = -10002,
       AMEDIA_ERROR_INVALID_OBJECT    = -10003,
       AMEDIA_ERROR_INVALID_PARAMETER = -10004 };
enum { AMEDIACODEC_INFO_TRY_AGAIN_LATER    = -1,
       AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2,
       AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3 };
enum { AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG = 2,
       AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4 };

typedef struct {
    int32_t  offset;
    int32_t  size;
    int64_t  presentationTimeUs;
    uint32_t flags;
} AMediaCodecBufferInfo;

// AImageReader_ImageListener, by layout: the guest builds one of these and
// hands us its address, and we read both fields out of it.
typedef void (*klm_image_cb)(void *context, void *reader);
typedef struct { void *context; klm_image_cb onImageAvailable; } klm_listener;

// ---------------------------------------------------------------------------
// AMediaFormat — a key/value bag, and nothing more.
//
// Values are held rather than interpreted. The only key we read is "mime", to
// pick a decoder; width/height come from the bitstream's own parameter sets,
// which is more authoritative than anything the guest can tell us, and the
// colour keys describe a conversion VideoToolbox is doing for us. Keeping the
// rest anyway means AMediaFormat_toString and the report can show what the
// guest asked for, which is how a wrong assumption gets noticed.
#define KLM_FMT_MAX 32
typedef struct {
    struct { char *key; char *sval; int32_t ival; int is_str; } kv[KLM_FMT_MAX];
    int n;
} klm_format;

static int klm_fmt_slot(klm_format *f, const char *key) {
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->kv[i].key, key)) return i;
    if (f->n == KLM_FMT_MAX) return -1;
    f->kv[f->n].key = strdup(key);
    return f->n++;
}

static void *klm_AMediaFormat_new(void) { return calloc(1, sizeof(klm_format)); }

static int klm_AMediaFormat_delete(void *fmt) {
    klm_format *f = fmt;
    if (!f) return AMEDIA_ERROR_INVALID_PARAMETER;
    for (int i = 0; i < f->n; i++) { free(f->kv[i].key); free(f->kv[i].sval); }
    free(f);
    return AMEDIA_OK;
}

static void klm_AMediaFormat_setInt32(void *fmt, const char *key, int32_t v) {
    klm_format *f = fmt;
    if (!f || !key) return;
    int i = klm_fmt_slot(f, key);
    if (i < 0) return;
    f->kv[i].ival = v; f->kv[i].is_str = 0;
}

static void klm_AMediaFormat_setString(void *fmt, const char *key, const char *v) {
    klm_format *f = fmt;
    if (!f || !key) return;
    int i = klm_fmt_slot(f, key);
    if (i < 0) return;
    // Copied, not pointed at: the guest builds these strings on its own stack
    // and in its own std::string temporaries. Trap 6's lesson, again.
    free(f->kv[i].sval);
    f->kv[i].sval = v ? strdup(v) : NULL;
    f->kv[i].is_str = 1;
}

static const char *klm_fmt_string(klm_format *f, const char *key) {
    if (!f) return NULL;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->kv[i].key, key) && f->kv[i].is_str) return f->kv[i].sval;
    return NULL;
}

// The getters. `false` means "this format does not carry that key", and the NDK
// contract is that the out parameter is then LEFT ALONE — the caller keeps its
// own default. Writing a zero on the false path is trap 10b's shape: a value
// the caller never asked for, indistinguishable from a measurement.
//
// Nothing here ever carries a key today (see the extractor below: it publishes
// no tracks, so no track format is ever built), which makes every one of these
// a truthful "no" rather than a stub. They exist because a format whose owner
// cannot fill it must still be ASKABLE — a guest querying an empty format is
// doing the right thing and must get an answer, not an abort.
static bool klm_AMediaFormat_getInt32(void *fmt, const char *key, int32_t *out) {
    klm_format *f = fmt;
    if (!f || !key || !out) return false;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->kv[i].key, key) && !f->kv[i].is_str) { *out = f->kv[i].ival; return true; }
    return false;
}
static bool klm_AMediaFormat_getInt64(void *fmt, const char *key, int64_t *out) {
    int32_t v;
    if (!out || !klm_AMediaFormat_getInt32(fmt, key, &v)) return false;
    *out = v;
    return true;
}
static bool klm_AMediaFormat_getFloat(void *fmt, const char *key, float *out) {
    int32_t v;
    if (!out || !klm_AMediaFormat_getInt32(fmt, key, &v)) return false;
    *out = (float)v;
    return true;
}
static bool klm_AMediaFormat_getString(void *fmt, const char *key, const char **out) {
    if (!out) return false;
    const char *s = klm_fmt_string(fmt, key);
    if (!s) return false;
    *out = s;
    return true;
}

// The format-key constants. These are DATA symbols in the NDK
// (`extern const char *AMEDIAFORMAT_KEY_MIME;`), so the shim exports the
// ADDRESS of each pointer and the guest loads through it — get this wrong and
// the guest passes a string that is really a pointer-to-pointer, which lands as
// a key nobody matches rather than as a crash.
static const char *g_key_mime            = "mime";
static const char *g_key_width           = "width";
static const char *g_key_height          = "height";
static const char *g_key_max_width       = "max-width";
static const char *g_key_max_height      = "max-height";
static const char *g_key_color_transfer  = "color-transfer";
static const char *g_key_color_standard  = "color-standard";
static const char *g_key_color_range     = "color-range";
static const char *g_key_color_format    = "color-format";
static const char *g_key_priority        = "priority";
static const char *g_key_operating_rate  = "operating-rate";
// ...and the ones only a DEMUXING guest names. Unity's VideoPlayer READS a
// track's format rather than describing one, so these arrive through the
// getters below rather than the setters above.
// Only the ones libunity imports STRONGLY. It imports rotation-degrees,
// slice-height and encoder-delay weakly, i.e. it is written to run on an
// Android version that does not define them and checks for NULL first — so
// defining them here would not be completeness, it would be turning on a code
// path the guest asked to be able to skip.
static const char *g_key_duration        = "durationUs";
static const char *g_key_frame_rate      = "frame-rate";
static const char *g_key_language        = "language";
static const char *g_key_channel_count   = "channel-count";
static const char *g_key_sample_rate     = "sample-rate";
static const char *g_key_stride          = "stride";

// ---------------------------------------------------------------------------
// AMediaExtractor — the DEMUXER, and the one part of this file that refuses.
//
// Unity's VideoPlayer on Android is extractor + codec: AMediaExtractor pulls
// compressed samples out of a container and hands them to AMediaCodec, which
// this file already serves over VideoToolbox. The codec half exists because
// Steam Link needed it (SL-10) and it arrives an elementary stream, already
// demuxed by the guest's own network protocol. Nothing here has ever opened a
// CONTAINER, and that is the whole gap: an .mp4 is a box structure that must be
// parsed to find where each sample begins.
//
// Open Brush is the first guest to reach it. It ships `animated-logo.mp4` and
// SEEDS it into its Media Library on first run, so its VideoCatalog scan
// prepares a VideoPlayer during startup whether or not the user ever opens the
// reference panel. Before this, that call was `AMediaExtractor_new` as an
// unresolved import, i.e. an abort — which is correct for something nobody has
// asked for yet, and wrong for something on the startup path of a working
// guest.
//
// So this is a REFUSAL, not a stub, and the difference is that a refusal is a
// legal answer the guest is already written to handle. A file it cannot demux
// is an ordinary condition for a media player — a corrupt download, a codec the
// device lacks — so VideoPlayer has an error path, reports through
// `errorReceived`, and Open Brush's ReferenceVideo simply leaves
// m_VideoInitialized false. The refusal is stated twice, at both points a
// caller might check: setDataSource returns UNSUPPORTED, and if the caller
// carries on regardless, the extractor then honestly has no tracks. Those two
// have to agree — an extractor that failed to open and then claims a track is a
// worse answer than either.
//
// What it would take to make real, recorded so the next person does not have to
// derive it: AVAssetReader over an AVURLAsset, with nil output settings so the
// samples come out still compressed, feeding the AMediaCodec path that is
// already here. That is the same split kl_vtdec/kl_mediandk already uses. It is
// a session of its own and it would land Unity's VideoPlayer for EVERY target
// (Beat Saber imports the same 41 symbols), which is why it is written down
// rather than half-started.
#define KLM_EXTRACTOR_MAGIC 0x4b4c4d58u   /* 'KLMX' */
typedef struct {
    uint32_t magic;
    char    *source;        // for the report: what it was asked to open
} klm_extractor;

static klm_extractor *klm_ex(void *ex) {
    klm_extractor *x = ex;
    return (x && x->magic == KLM_EXTRACTOR_MAGIC) ? x : NULL;
}

// Counted so the end-of-run report can say a video was wanted and refused,
// rather than leaving "no picture" to be discovered by looking at the screen.
// The name outlives the extractor, which the guest deletes as soon as it gives
// up — so it is copied here rather than read back off an object at report time.
static unsigned g_ex_refused;
static char    *g_ex_last;

static void *klm_AMediaExtractor_new(void) {
    klm_extractor *x = calloc(1, sizeof *x);
    if (x) x->magic = KLM_EXTRACTOR_MAGIC;
    return x;
}

static int klm_AMediaExtractor_delete(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x) return AMEDIA_ERROR_INVALID_OBJECT;
    free(x->source);
    x->magic = 0;
    free(x);
    return AMEDIA_OK;
}

static int klm_extractor_refuse(klm_extractor *x, const char *what) {
    free(x->source);
    x->source = what ? strdup(what) : NULL;
    free(g_ex_last);
    g_ex_last = what ? strdup(what) : NULL;
    if (!g_ex_refused++)
        fprintf(stderr, "  [media] AMediaExtractor: no container demuxer here — "
                        "refusing \"%s\". The guest's own no-video path runs from "
                        "here (see the note in kl_mediandk.c).\n",
                what ? what : "(fd)");
    return AMEDIA_ERROR_UNSUPPORTED;
}

static int klm_AMediaExtractor_setDataSource(void *ex, const char *location) {
    klm_extractor *x = klm_ex(ex);
    if (!x) return AMEDIA_ERROR_INVALID_OBJECT;
    return klm_extractor_refuse(x, location);
}

static int klm_AMediaExtractor_setDataSourceFd(void *ex, int fd, int64_t offset, int64_t length) {
    (void)offset; (void)length;
    klm_extractor *x = klm_ex(ex);
    if (!x) return AMEDIA_ERROR_INVALID_OBJECT;
    // The fd's own path if the kernel will give it — a refusal that names the
    // file is worth more in a log than one that names a descriptor number.
    char path[1024];
    int named = fd >= 0 && fcntl(fd, F_GETPATH, path) == 0;
    return klm_extractor_refuse(x, named ? path : NULL);
}

// Everything past the refusal describes an extractor with nothing in it. These
// are the values the NDK defines for "no more samples" / "no such track", not
// invented ones, so a guest that ignored the setDataSource status still walks a
// consistent empty stream rather than reading uninitialised memory.
static size_t  klm_AMediaExtractor_getTrackCount(void *ex)      { (void)ex; return 0; }
static void   *klm_AMediaExtractor_getTrackFormat(void *ex, size_t i) { (void)ex; (void)i; return NULL; }
static int     klm_AMediaExtractor_selectTrack(void *ex, size_t i) {
    (void)ex; (void)i; return AMEDIA_ERROR_INVALID_PARAMETER;
}
static ssize_t klm_AMediaExtractor_readSampleData(void *ex, uint8_t *buf, size_t cap) {
    (void)ex; (void)buf; (void)cap; return -1;      // -1 is end of stream
}
static int     klm_AMediaExtractor_getSampleTrackIndex(void *ex) { (void)ex; return -1; }
static int64_t klm_AMediaExtractor_getSampleTime(void *ex)       { (void)ex; return -1; }
static bool    klm_AMediaExtractor_advance(void *ex)             { (void)ex; return false; }
static int     klm_AMediaExtractor_seekTo(void *ex, int64_t us, int mode) {
    (void)ex; (void)us; (void)mode; return AMEDIA_ERROR_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// AHardwareBuffer — a refcounted handle on one CVPixelBuffer.

typedef struct {
    uint32_t magic;
    int      refs;
    void    *pixels;            // CVPixelBufferRef, retained
} klm_hwbuf;

#define KLM_HWBUF_MAGIC 0x4b4c4842u    /* 'KLHB' */

// CoreVideo through function pointers would be silly here; kl_vtdec.h already
// pulls CoreVideo in, and this file is Darwin-only for the same reason it is.
#include <CoreVideo/CoreVideo.h>

static klm_hwbuf *klm_hwbuf_new(CVPixelBufferRef pb) {
    klm_hwbuf *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->magic = KLM_HWBUF_MAGIC;
    h->refs = 1;
    h->pixels = (void *)CVPixelBufferRetain(pb);
    return h;
}

static void klm_hwbuf_release(klm_hwbuf *h) {
    if (!h || h->magic != KLM_HWBUF_MAGIC) return;
    if (__atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    CVPixelBufferRelease((CVPixelBufferRef)h->pixels);
    h->magic = 0;
    free(h);
}

void *kl_mediandk_buffer_pixels(const void *buf) {
    const klm_hwbuf *h = buf;
    if (!h || h->magic != KLM_HWBUF_MAGIC) return NULL;
    return h->pixels;
}

static void klm_AHardwareBuffer_acquire(void *buf) {
    klm_hwbuf *h = buf;
    if (h && h->magic == KLM_HWBUF_MAGIC)
        __atomic_add_fetch(&h->refs, 1, __ATOMIC_RELAXED);
}
static void klm_AHardwareBuffer_release(void *buf) { klm_hwbuf_release(buf); }

// ---------------------------------------------------------------------------
// AImage — one frame, taken off the reader's queue.

typedef struct {
    uint32_t magic;
    void    *pixels;            // CVPixelBufferRef, retained
    int64_t  pts_us;
    klm_hwbuf *hw;              // made on demand, owned by the image
} klm_image;

#define KLM_IMAGE_MAGIC 0x4b4c494du    /* 'KLIM' */

static klm_image *klm_image_new(CVPixelBufferRef pb, int64_t pts_us) {
    klm_image *im = calloc(1, sizeof *im);
    if (!im) return NULL;
    im->magic = KLM_IMAGE_MAGIC;
    im->pixels = (void *)CVPixelBufferRetain(pb);
    im->pts_us = pts_us;
    return im;
}

static void klm_AImage_delete(void *image) {
    klm_image *im = image;
    if (!im || im->magic != KLM_IMAGE_MAGIC) return;
    // The hardware buffer deliberately outlives the image: the guest's own
    // sequence is acquire the image, take its buffer, AHardwareBuffer_acquire,
    // delete the image, and use the buffer for several frames afterwards
    // (QSVLFrameServer keeps a deque of them). Releasing our reference here and
    // letting the guest's keep it alive is exactly Android's contract.
    klm_hwbuf_release(im->hw);
    CVPixelBufferRelease((CVPixelBufferRef)im->pixels);
    im->magic = 0;
    free(im);
}

static int klm_AImage_getWidth(void *image, int32_t *out) {
    klm_image *im = image;
    if (!im || im->magic != KLM_IMAGE_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = (int32_t)CVPixelBufferGetWidth((CVPixelBufferRef)im->pixels);
    return AMEDIA_OK;
}
static int klm_AImage_getHeight(void *image, int32_t *out) {
    klm_image *im = image;
    if (!im || im->magic != KLM_IMAGE_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = (int32_t)CVPixelBufferGetHeight((CVPixelBufferRef)im->pixels);
    return AMEDIA_OK;
}
// NANOseconds here, microseconds everywhere else in this file. AImage's
// timestamp is the one place the NDK changes unit, and the guest uses it to
// pace presentation — reporting microseconds would make every frame look 1000x
// too early and is the kind of thing that reads as a network problem.
static int klm_AImage_getTimestamp(void *image, int64_t *out) {
    klm_image *im = image;
    if (!im || im->magic != KLM_IMAGE_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = im->pts_us * 1000;
    return AMEDIA_OK;
}
// The frame path is six handoffs long — submit, dequeue, release(render),
// publish, acquire, hardware buffer — and any one of them not happening looks
// exactly like any other from outside: no picture. So each one says so the first
// time it happens, and the run's log then names the LAST rung reached rather
// than leaving the whole ladder to be inferred from an end-of-run count.
#define KLM_FIRST(flagvar, ...) do { \
        static int flagvar; \
        if (!flagvar++) { fprintf(stderr, "  [media] " __VA_ARGS__); } \
    } while (0)

static int klm_AImage_getHardwareBuffer(void *image, void **out) {
    klm_image *im = image;
    if (!im || im->magic != KLM_IMAGE_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (!im->hw) im->hw = klm_hwbuf_new((CVPixelBufferRef)im->pixels);
    if (!im->hw) return AMEDIA_ERROR_UNKNOWN;
    *out = im->hw;
    KLM_FIRST(said_hw, "first AHardwareBuffer handed out (%p) — the guest can now "
                       "ask EGL for an image of it\n", (void *)im->hw);
    return AMEDIA_OK;
}

// ---------------------------------------------------------------------------
// AImageReader — the codec's output surface, and the queue behind it.

// The queue behind the reader. This guest asks for 20 images and its
// QSVLFrameServer really does hold a deque of them, so a pool sized to a
// plausible-looking 8 would silently drop frames at exactly the moment the
// stream is healthiest — SL-9's swapchain-pool lesson, which cost a session
// then. Sized above the request, and the request is reported.
#define KLM_READER_MAX 32

typedef struct {
    uint32_t magic;
    int32_t  width, height, format;
    uint64_t usage;
    int32_t  max_images;

    void    *window;            // what getWindow hands out; a kl_ndk window

    klm_listener listener;      // copied out of the guest's struct
    int      have_listener;

    pthread_mutex_t lock;
    pthread_cond_t  cv;
    struct { void *pb; int64_t pts_us; } q[KLM_READER_MAX];
    int      head, count;
    unsigned n_queued, n_acquired, n_dropped;

    pthread_t dispatch;
    int       running;
    int       pending;          // listener calls owed to the guest
} klm_reader;

#define KLM_READER_MAGIC 0x4b4c5244u   /* 'KLRD' */

static klm_reader *g_reader;        // for the report; this guest makes one
// Lifetime totals. The per-object counters die with the object, and the run
// where "did anything decode?" matters most is the CLEAN one — where the app
// shuts down and deletes both the codec and the reader before anything gets
// to report. These outlive that. (SL-11: an empty media report read as "the
// decoder was never fed" when it only meant "teardown got there first".)
static unsigned g_life_in, g_life_out, g_life_rendered, g_life_published, g_life_acquired;
// ...and the one bit that says the report is worth printing at all: a codec or a
// reader existed at some point. Counters can legitimately all be zero, and that
// is the single most interesting thing a report can say.
static int g_ever_created;

// Android delivers onImageAvailable on a thread that is NOT the one that
// released the buffer, and that is load-bearing rather than incidental. This
// guest's HandleOnImageAvailable takes the same codec mutex its render loop
// holds across AMediaCodec_releaseOutputBuffer, so calling the listener inline
// from release would re-enter that mutex and wedge — the "a HandlerThread needs
// a real thread" lesson from M4, in a new subsystem. One thread per reader,
// woken by the queue.
static void *reader_dispatch(void *arg) {
    klm_reader *r = arg;
    // Guest code runs on this thread, so the S0.1/S0.5 per-thread setup has to
    // happen before the first call into it.
    kl_thread_init();
    pthread_mutex_lock(&r->lock);
    while (r->running) {
        while (r->running && !r->pending)
            pthread_cond_wait(&r->cv, &r->lock);
        if (!r->running) break;
        r->pending--;
        klm_listener l = r->listener;
        int have = r->have_listener;
        pthread_mutex_unlock(&r->lock);
        if (have && l.onImageAvailable) {
            KLM_FIRST(said_cb, "first onImageAvailable delivered to the guest\n");
            l.onImageAvailable(l.context, r);
        }
        pthread_mutex_lock(&r->lock);
    }
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

static int klm_AImageReader_newWithUsage(int32_t w, int32_t h, int32_t format,
                                         uint64_t usage, int32_t max_images,
                                         void **out) {
    if (!out || max_images < 1) return AMEDIA_ERROR_INVALID_PARAMETER;
    klm_reader *r = calloc(1, sizeof *r);
    if (!r) return AMEDIA_ERROR_UNKNOWN;
    r->magic = KLM_READER_MAGIC;
    r->width = w; r->height = h; r->format = format;
    r->usage = usage;
    r->max_images = max_images < KLM_READER_MAX ? max_images : KLM_READER_MAX;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cv, NULL);

    // The window the codec will render into. Its size is the reader's for now
    // and is corrected the moment the decoder reports the real one — the guest
    // asks for 1x1 here deliberately (see the header), so believing it would
    // publish a 1x1 surface.
    r->window = kl_ndk_window_new(w, h, format, r);

    r->running = 1;
    if (pthread_create(&r->dispatch, NULL, reader_dispatch, r) != 0) {
        r->running = 0;
        fprintf(stderr, "  [media] AImageReader: could not start the dispatch "
                        "thread; onImageAvailable will never fire\n");
    }
    *out = r;
    g_reader = r;
    fprintf(stderr, "  [media] AImageReader %dx%d format 0x%x usage 0x%llx, "
                    "%d images\n", w, h, format, (unsigned long long)usage,
            max_images);
    return AMEDIA_OK;
}

static int klm_AImageReader_setImageListener(void *reader, void *listener) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&r->lock);
    if (listener) {
        // Copied out by value. The guest's own struct is a member of an object
        // it may move or destroy, and Android copies it too.
        r->listener = *(const klm_listener *)listener;
        r->have_listener = 1;
    } else {
        r->have_listener = 0;
    }
    pthread_mutex_unlock(&r->lock);
    return AMEDIA_OK;
}

static int klm_AImageReader_getWindow(void *reader, void **out) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = r->window;
    return AMEDIA_OK;
}

// "Latest" is the whole contract: hand back the newest frame and drop
// everything older, because for live video an older frame has no value and
// holding it only delays the next one. Dropping silently would read as a
// decoder that is behind, so the count is reported.
static int klm_AImageReader_acquireLatestImage(void *reader, void **out) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&r->lock);
    if (!r->count) { pthread_mutex_unlock(&r->lock); return AMEDIA_ERROR_UNKNOWN; }
    while (r->count > 1) {
        CVPixelBufferRelease((CVPixelBufferRef)r->q[r->head].pb);
        r->head = (r->head + 1) % KLM_READER_MAX;
        r->count--;
        r->n_dropped++;
    }
    void *pb = r->q[r->head].pb;
    int64_t pts = r->q[r->head].pts_us;
    r->head = (r->head + 1) % KLM_READER_MAX;
    r->count--;
    r->n_acquired++;
    pthread_mutex_unlock(&r->lock);
    KLM_FIRST(said_acq, "first acquireLatestImage (pts %lld us)\n", (long long)pts);

    klm_image *im = klm_image_new((CVPixelBufferRef)pb, pts);
    CVPixelBufferRelease((CVPixelBufferRef)pb);   // the image took its own
    if (!im) return AMEDIA_ERROR_UNKNOWN;
    *out = im;
    return AMEDIA_OK;
}

static void klm_AImageReader_delete(void *reader) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC) return;
    pthread_mutex_lock(&r->lock);
    r->running = 0;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->lock);
    pthread_join(r->dispatch, NULL);
    while (r->count) {
        CVPixelBufferRelease((CVPixelBufferRef)r->q[r->head].pb);
        r->head = (r->head + 1) % KLM_READER_MAX;
        r->count--;
    }
    kl_ndk_window_free(r->window);
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->cv);
    g_life_published += r->n_queued; g_life_acquired += r->n_acquired;
    if (g_reader == r) g_reader = NULL;
    r->magic = 0;
    free(r);
}

// Called by the codec when a frame is released with render=true.
static void klm_reader_publish(klm_reader *r, CVPixelBufferRef pb, int64_t pts_us) {
    if (!r || r->magic != KLM_READER_MAGIC) return;
    pthread_mutex_lock(&r->lock);
    if (r->count == KLM_READER_MAX) {
        CVPixelBufferRelease((CVPixelBufferRef)r->q[r->head].pb);
        r->head = (r->head + 1) % KLM_READER_MAX;
        r->count--;
        r->n_dropped++;
    }
    int slot = (r->head + r->count) % KLM_READER_MAX;
    r->q[slot].pb = (void *)CVPixelBufferRetain(pb);
    r->q[slot].pts_us = pts_us;
    r->count++;
    r->n_queued++;
    r->pending++;
    // The window's size is the producer's, not the 1x1 the guest asked for.
    // Publishing it here rather than at configure time is what makes it the
    // decoder's answer.
    kl_ndk_window_set_size(r->window, (int32_t)CVPixelBufferGetWidth(pb),
                           (int32_t)CVPixelBufferGetHeight(pb));
    KLM_FIRST(said_pub, "first frame published to the reader: %zux%zu, pts %lld us "
                        "— onImageAvailable fires next\n",
              CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb),
              (long long)pts_us);
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->lock);
}

// ---------------------------------------------------------------------------
// AMediaCodec

#define KLM_IN_BUFFERS  8
#define KLM_IN_CAP      (2u << 20)      // per input buffer; a 4K keyframe fits
#define KLM_OUT_SLOTS   16

typedef struct {
    uint32_t magic;
    char     mime[32];
    kl_vtdec *dec;
    klm_reader *surface;
    int      started;

    pthread_mutex_t lock;
    struct { uint8_t *data; int in_use; } in[KLM_IN_BUFFERS];
    struct { void *pb; int64_t pts_us; int in_use; } out[KLM_OUT_SLOTS];

    // The decoded size, learned from the first frame rather than from
    // configure(): the bitstream's own parameter sets are more authoritative
    // than anything the guest can say, and until a frame exists there is
    // nothing to report. AMediaCodec_getOutputFormat reads these.
    int      out_w, out_h;

    unsigned n_in, n_out, n_rendered;
} klm_codec;

#define KLM_CODEC_MAGIC 0x4b4c4344u    /* 'KLCD' */

static klm_codec *g_codec;


static void *klm_AMediaCodec_createDecoderByType(const char *mime) {
    klm_codec *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->magic = KLM_CODEC_MAGIC;
    snprintf(c->mime, sizeof c->mime, "%s", mime ? mime : "");
    pthread_mutex_init(&c->lock, NULL);
    c->dec = kl_vtdec_create(mime);
    if (!c->dec) {
        // Not an abort. "No decoder for this type" is a state MediaCodec has,
        // the guest checks for it, and its own message is better than ours.
        fprintf(stderr, "  [media] AMediaCodec_createDecoderByType(\"%s\"): "
                        "no host decoder for that type\n", mime ? mime : "(null)");
        pthread_mutex_destroy(&c->lock);
        free(c);
        return NULL;
    }
    fprintf(stderr, "  [media] AMediaCodec_createDecoderByType(\"%s\")\n", mime);
    g_codec = c;
    g_ever_created = 1;
    return c;
}

static int klm_AMediaCodec_configure(void *codec, const void *format,
                                     void *window, void *crypto, uint32_t flags) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (crypto) {
        // A decrypting configure is DRM, and this project does not do that
        // (CLAUDE.md, "the DRM line"). Steam Link never passes one; refusing
        // rather than ignoring keeps it that way.
        fprintf(stderr, "  [media] AMediaCodec_configure with an AMediaCrypto — "
                        "refused\n");
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    const char *mime = klm_fmt_string((klm_format *)format, "mime");
    // The surface is the AImageReader's window; that is the only kind this
    // guest passes, and the owner pointer the window carries is what connects
    // the two objects back up.
    klm_reader *r = kl_ndk_window_owner(window);
    c->surface = r;
    fprintf(stderr, "  [media] AMediaCodec_configure: mime=%s surface=%p flags=0x%x\n",
            mime ? mime : "(unset)", window, flags);
    if (window && !r)
        fprintf(stderr, "  [media]   ...that window is not an AImageReader's; "
                        "rendered frames will have nowhere to go\n");
    return AMEDIA_OK;
}

static int klm_AMediaCodec_start(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    c->started = 1;
    return AMEDIA_OK;
}

static int klm_AMediaCodec_stop(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    c->started = 0;
    return AMEDIA_OK;
}

static void klm_codec_drop_outputs(klm_codec *c) {
    for (int i = 0; i < KLM_OUT_SLOTS; i++)
        if (c->out[i].in_use) {
            CVPixelBufferRelease((CVPixelBufferRef)c->out[i].pb);
            c->out[i].pb = NULL;
            c->out[i].in_use = 0;
        }
}

static int klm_AMediaCodec_flush(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&c->lock);
    klm_codec_drop_outputs(c);
    for (int i = 0; i < KLM_IN_BUFFERS; i++) c->in[i].in_use = 0;
    pthread_mutex_unlock(&c->lock);
    kl_vtdec_flush(c->dec);
    return AMEDIA_OK;
}

static int klm_AMediaCodec_delete(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&c->lock);
    klm_codec_drop_outputs(c);
    for (int i = 0; i < KLM_IN_BUFFERS; i++) free(c->in[i].data);
    pthread_mutex_unlock(&c->lock);
    kl_vtdec_destroy(c->dec);
    pthread_mutex_destroy(&c->lock);
    g_life_in += c->n_in; g_life_out += c->n_out; g_life_rendered += c->n_rendered;
    if (g_codec == c) g_codec = NULL;
    c->magic = 0;
    free(c);
    return AMEDIA_OK;
}

// timeoutUs: negative means block indefinitely, 0 means poll. We always have a
// buffer unless the guest has dequeued all eight without queueing any, so the
// wait is a formality — but honouring it costs nothing and a guest that passes
// -1 expecting to block must not get an instant refusal it reads as an error.
static ssize_t klm_AMediaCodec_dequeueInputBuffer(void *codec, int64_t timeoutUs) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    int64_t waited = 0;
    for (;;) {
        pthread_mutex_lock(&c->lock);
        for (int i = 0; i < KLM_IN_BUFFERS; i++)
            if (!c->in[i].in_use) {
                c->in[i].in_use = 1;
                pthread_mutex_unlock(&c->lock);
                return i;
            }
        pthread_mutex_unlock(&c->lock);
        if (timeoutUs == 0 || (timeoutUs > 0 && waited >= timeoutUs))
            return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        usleep(1000);
        waited += 1000;
    }
}

static uint8_t *klm_AMediaCodec_getInputBuffer(void *codec, size_t idx,
                                               size_t *out_size) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC || idx >= KLM_IN_BUFFERS) return NULL;
    pthread_mutex_lock(&c->lock);
    if (!c->in[idx].data) c->in[idx].data = malloc(KLM_IN_CAP);
    uint8_t *p = c->in[idx].data;
    pthread_mutex_unlock(&c->lock);
    if (out_size) *out_size = p ? KLM_IN_CAP : 0;
    return p;
}

// The OUTPUT side's two, and both answers follow from this codec being
// surface-configured rather than from anything missing.
//
// getOutputBuffer is NULL because a codec configured with a surface has no
// client-visible output buffers at all — the frames go to the surface, which
// here is the AImageReader above. That is Android's own answer for the same
// configuration, not a gap: a guest that gets NULL here is being told to read
// the surface, which is exactly what Unity does.
static uint8_t *klm_AMediaCodec_getOutputBuffer(void *codec, size_t idx, size_t *out_size) {
    klm_codec *c = codec;
    (void)idx;
    if (out_size) *out_size = 0;
    if (!c || c->magic != KLM_CODEC_MAGIC) return NULL;
    KLM_FIRST(said_outbuf, "AMediaCodec_getOutputBuffer on a surface-configured "
                           "codec — NULL, as on Android; the frames are on the "
                           "reader\n");
    return NULL;
}

// getOutputFormat hands back a NEW format the caller owns and deletes. What it
// can honestly carry is what the DECODER measured, not what the guest asked for
// — so the size is the first decoded frame's, and BEFORE that first frame the
// format carries no size at all. That is not a gap: MediaCodec's own output
// format is only meaningful once the codec has reported
// INFO_OUTPUT_FORMAT_CHANGED, and the getters answer "absent" by leaving the
// caller's own default in place rather than by writing a zero over it.
static void *klm_AMediaCodec_getOutputFormat(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return NULL;
    klm_format *f = klm_AMediaFormat_new();
    if (!f) return NULL;
    klm_AMediaFormat_setString(f, g_key_mime, c->mime);
    pthread_mutex_lock(&c->lock);
    int w = c->out_w, h = c->out_h;
    pthread_mutex_unlock(&c->lock);
    if (w && h) {
        klm_AMediaFormat_setInt32(f, g_key_width,  w);
        klm_AMediaFormat_setInt32(f, g_key_height, h);
    }
    // Not the stride: the frames never pass through a linear buffer here (see
    // getOutputBuffer), so there is no row pitch to report and reporting one
    // would describe memory the guest cannot reach.
    return f;
}

static int klm_AMediaCodec_queueInputBuffer(void *codec, size_t idx, off_t offset,
                                            size_t size, uint64_t time_us,
                                            uint32_t flags) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC || idx >= KLM_IN_BUFFERS)
        return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&c->lock);
    uint8_t *p = c->in[idx].data;
    c->in[idx].in_use = 0;
    c->n_in++;
    pthread_mutex_unlock(&c->lock);
    if (!p) return AMEDIA_ERROR_UNKNOWN;
    KLM_FIRST(said_in, "first access unit queued: %zu bytes, pts %llu us, flags 0x%x\n",
              size, (unsigned long long)time_us, flags);
    if (offset < 0 || size > KLM_IN_CAP || (size_t)offset > KLM_IN_CAP - size)
        return AMEDIA_ERROR_INVALID_PARAMETER;
    if (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return AMEDIA_OK;
    // A codec-config buffer is parameter sets and nothing else, which is
    // exactly what kl_vtdec absorbs without producing a frame. No special case
    // is needed here; the flag is checked only so an empty one is not an error.
    if (!size) return (flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) ? AMEDIA_OK
                                                                    : AMEDIA_ERROR_INVALID_PARAMETER;
    return kl_vtdec_submit(c->dec, p + offset, size, (int64_t)time_us) == 0
             ? AMEDIA_OK : AMEDIA_ERROR_UNKNOWN;
}

static ssize_t klm_AMediaCodec_dequeueOutputBuffer(void *codec,
                                                   AMediaCodecBufferInfo *info,
                                                   int64_t timeoutUs) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    int64_t waited = 0;
    for (;;) {
        int64_t pts = 0;
        CVPixelBufferRef pb = kl_vtdec_pull(c->dec, &pts);
        if (pb) {
            pthread_mutex_lock(&c->lock);
            for (int i = 0; i < KLM_OUT_SLOTS; i++)
                if (!c->out[i].in_use) {
                    c->out[i].in_use = 1;
                    c->out[i].pb = (void *)pb;
                    c->out[i].pts_us = pts;
                    c->out_w = (int)CVPixelBufferGetWidth(pb);
                    c->out_h = (int)CVPixelBufferGetHeight(pb);
                    c->n_out++;
                    pthread_mutex_unlock(&c->lock);
                    KLM_FIRST(said_out, "first decoded frame dequeued (slot %d, "
                                        "pts %lld us)\n", i, (long long)pts);
                    if (info) {
                        info->offset = 0;
                        // Zero, and deliberately: this codec renders to a
                        // surface, so there is no CPU-visible output buffer and
                        // Android reports 0 too. A guest that memcpy'd `size`
                        // bytes out of it would be reading a buffer that does
                        // not exist on Android either.
                        info->size = 0;
                        info->presentationTimeUs = pts;
                        info->flags = 0;
                    }
                    return i;
                }
            // Every slot outstanding: the guest is not releasing. Put it back
            // rather than dropping it, and tell the guest to come back.
            pthread_mutex_unlock(&c->lock);
            CVPixelBufferRelease(pb);
            return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        }
        if (timeoutUs == 0 || (timeoutUs > 0 && waited >= timeoutUs))
            return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        usleep(1000);
        waited += 1000;
    }
}

static int klm_AMediaCodec_releaseOutputBuffer(void *codec, size_t idx, bool render) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC || idx >= KLM_OUT_SLOTS)
        return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&c->lock);
    if (!c->out[idx].in_use) { pthread_mutex_unlock(&c->lock); return AMEDIA_ERROR_INVALID_PARAMETER; }
    CVPixelBufferRef pb = (CVPixelBufferRef)c->out[idx].pb;
    int64_t pts = c->out[idx].pts_us;
    klm_reader *r = c->surface;
    c->out[idx].in_use = 0;
    c->out[idx].pb = NULL;
    if (render) c->n_rendered++;
    pthread_mutex_unlock(&c->lock);

    if (render) KLM_FIRST(said_rel, "first releaseOutputBuffer(render=true)%s\n",
                          r ? "" : " — but the codec has no output surface, "
                                   "so the frame goes nowhere");
    if (render && r) klm_reader_publish(r, pb, pts);
    CVPixelBufferRelease(pb);
    return AMEDIA_OK;
}

// ---------------------------------------------------------------------------
// ATrace. Two calls, no state, and they are on the guest's per-frame path — the
// point of serving them is that they are free and their absence is not.
static void klm_ATrace_beginSection(const char *name) { (void)name; }
static void klm_ATrace_endSection(void) { }

// ---------------------------------------------------------------------------

void kl_mediandk_report(FILE *f) {
    unsigned in = g_life_in + (g_codec ? g_codec->n_in : 0);
    unsigned out = g_life_out + (g_codec ? g_codec->n_out : 0);
    unsigned rendered = g_life_rendered + (g_codec ? g_codec->n_rendered : 0);
    unsigned published = g_life_published + (g_reader ? g_reader->n_queued : 0);
    unsigned acquired = g_life_acquired + (g_reader ? g_reader->n_acquired : 0);
    // Never silent once anything media-shaped has happened. "Did a frame reach
    // the guest?" is the question this run exists to answer, and a report that
    // prints nothing is indistinguishable from a report that never ran — which
    // is exactly how it read the first time (trap 6d, in the reporting path).
    // ...and the demuxer's refusals count as "media-shaped", because a guest
    // that asked for a video and was refused is the case most likely to be
    // investigated as a rendering problem. `g_ever_created` is false on that
    // path — no codec is ever built — so it has to be tested separately or the
    // refusal is invisible in exactly the run that needs it.
    if (!g_ever_created && !g_ex_refused) return;
    fprintf(f, "\n=== media (AMediaCodec / AImageReader) ===\n");
    if (g_ex_refused)
        fprintf(f, "  extractor: %u source(s) REFUSED — there is no container "
                   "demuxer here%s%s. The guest's own no-video path ran.\n",
                g_ex_refused, g_ex_last ? ", last: " : "", g_ex_last ? g_ex_last : "");
    if (!g_ever_created) return;
    fprintf(f, "  codec \"%s\": %u buffers queued, %u frames dequeued, %u rendered%s\n",
            g_codec ? g_codec->mime : "video/hevc", in, out, rendered,
            g_codec ? "" : "  (codec deleted; totals are for the whole run)");
    fprintf(f, "  reader: %u published, %u acquired, %u dropped as stale, %d held\n",
            published, acquired, g_reader ? g_reader->n_dropped : 0,
            g_reader ? g_reader->count : 0);
    kl_vtdec_report(f);
}

// ---------------------------------------------------------------------------
// The table, and the two doors into it.

#define M(sym, fn) {sym, (void *)(fn)}
static const struct { const char *name; void *fn; } g_media[] = {
    M("AMediaFormat_new",       klm_AMediaFormat_new),
    M("AMediaFormat_delete",    klm_AMediaFormat_delete),
    M("AMediaFormat_setInt32",  klm_AMediaFormat_setInt32),
    M("AMediaFormat_setString", klm_AMediaFormat_setString),
    M("AMediaFormat_getInt32",  klm_AMediaFormat_getInt32),
    M("AMediaFormat_getInt64",  klm_AMediaFormat_getInt64),
    M("AMediaFormat_getFloat",  klm_AMediaFormat_getFloat),
    M("AMediaFormat_getString", klm_AMediaFormat_getString),

    // Data symbols, not functions — see the note by the definitions.
    M("AMEDIAFORMAT_KEY_MIME",           &g_key_mime),
    M("AMEDIAFORMAT_KEY_WIDTH",          &g_key_width),
    M("AMEDIAFORMAT_KEY_HEIGHT",         &g_key_height),
    M("AMEDIAFORMAT_KEY_MAX_WIDTH",      &g_key_max_width),
    M("AMEDIAFORMAT_KEY_MAX_HEIGHT",     &g_key_max_height),
    M("AMEDIAFORMAT_KEY_COLOR_TRANSFER", &g_key_color_transfer),
    M("AMEDIAFORMAT_KEY_COLOR_STANDARD", &g_key_color_standard),
    M("AMEDIAFORMAT_KEY_COLOR_RANGE",    &g_key_color_range),
    M("AMEDIAFORMAT_KEY_COLOR_FORMAT",   &g_key_color_format),
    M("AMEDIAFORMAT_KEY_PRIORITY",       &g_key_priority),
    M("AMEDIAFORMAT_KEY_OPERATING_RATE", &g_key_operating_rate),
    M("AMEDIAFORMAT_KEY_DURATION",       &g_key_duration),
    M("AMEDIAFORMAT_KEY_FRAME_RATE",     &g_key_frame_rate),
    M("AMEDIAFORMAT_KEY_LANGUAGE",       &g_key_language),
    M("AMEDIAFORMAT_KEY_CHANNEL_COUNT",  &g_key_channel_count),
    M("AMEDIAFORMAT_KEY_SAMPLE_RATE",    &g_key_sample_rate),
    M("AMEDIAFORMAT_KEY_STRIDE",         &g_key_stride),

    M("AMediaCodec_createDecoderByType",  klm_AMediaCodec_createDecoderByType),
    M("AMediaCodec_configure",            klm_AMediaCodec_configure),
    M("AMediaCodec_start",                klm_AMediaCodec_start),
    M("AMediaCodec_stop",                 klm_AMediaCodec_stop),
    M("AMediaCodec_flush",                klm_AMediaCodec_flush),
    M("AMediaCodec_delete",               klm_AMediaCodec_delete),
    M("AMediaCodec_dequeueInputBuffer",   klm_AMediaCodec_dequeueInputBuffer),
    M("AMediaCodec_getInputBuffer",       klm_AMediaCodec_getInputBuffer),
    M("AMediaCodec_queueInputBuffer",     klm_AMediaCodec_queueInputBuffer),
    M("AMediaCodec_dequeueOutputBuffer",  klm_AMediaCodec_dequeueOutputBuffer),
    M("AMediaCodec_releaseOutputBuffer",  klm_AMediaCodec_releaseOutputBuffer),
    M("AMediaCodec_getOutputBuffer",      klm_AMediaCodec_getOutputBuffer),
    M("AMediaCodec_getOutputFormat",      klm_AMediaCodec_getOutputFormat),

    // The demuxer, which REFUSES — see the long note by the implementation.
    M("AMediaExtractor_new",                 klm_AMediaExtractor_new),
    M("AMediaExtractor_delete",              klm_AMediaExtractor_delete),
    M("AMediaExtractor_setDataSource",       klm_AMediaExtractor_setDataSource),
    M("AMediaExtractor_setDataSourceFd",     klm_AMediaExtractor_setDataSourceFd),
    M("AMediaExtractor_getTrackCount",       klm_AMediaExtractor_getTrackCount),
    M("AMediaExtractor_getTrackFormat",      klm_AMediaExtractor_getTrackFormat),
    M("AMediaExtractor_selectTrack",         klm_AMediaExtractor_selectTrack),
    M("AMediaExtractor_readSampleData",      klm_AMediaExtractor_readSampleData),
    M("AMediaExtractor_getSampleTrackIndex", klm_AMediaExtractor_getSampleTrackIndex),
    M("AMediaExtractor_getSampleTime",       klm_AMediaExtractor_getSampleTime),
    M("AMediaExtractor_advance",             klm_AMediaExtractor_advance),
    M("AMediaExtractor_seekTo",              klm_AMediaExtractor_seekTo),

    M("AImageReader_newWithUsage",       klm_AImageReader_newWithUsage),
    M("AImageReader_setImageListener",   klm_AImageReader_setImageListener),
    M("AImageReader_getWindow",          klm_AImageReader_getWindow),
    M("AImageReader_acquireLatestImage", klm_AImageReader_acquireLatestImage),
    M("AImageReader_delete",             klm_AImageReader_delete),

    M("AImage_getWidth",          klm_AImage_getWidth),
    M("AImage_getHeight",         klm_AImage_getHeight),
    M("AImage_getTimestamp",      klm_AImage_getTimestamp),
    M("AImage_getHardwareBuffer", klm_AImage_getHardwareBuffer),
    M("AImage_delete",            klm_AImage_delete),

    M("AHardwareBuffer_acquire", klm_AHardwareBuffer_acquire),
    M("AHardwareBuffer_release", klm_AHardwareBuffer_release),

    M("ATrace_beginSection", klm_ATrace_beginSection),
    M("ATrace_endSection",   klm_ATrace_endSection),
};

void *kl_mediandk_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_media / sizeof g_media[0]; i++)
        if (!strcmp(g_media[i].name, name)) return g_media[i].fn;
    return NULL;                // must be able to say no — see the header
}

// ---------------------------------------------------------------------------
// The dlopen door. Beat Saber's FMOD never asks; this exists because a guest
// that dlopens libmediandk.so must get a handle it can dlsym through, and
// because libOpenMAXAL.so has no implementation at all and should say so by
// name rather than by null pointer.

static const char g_md_handle[]  = "klepton-mediandk";
static const char g_oma_handle[] = "klepton-openmaxal";

int kl_mediandk_claims(const char *soname) {
    if (!soname) return 0;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    return strcmp(b, "libmediandk.so") == 0 || strcmp(b, "libOpenMAXAL.so") == 0;
}

void *kl_mediandk_dlopen(const char *soname) {
    if (!kl_mediandk_claims(soname)) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    if (strcmp(b, "libmediandk.so") == 0) return (void *)g_md_handle;
    fprintf(stderr, "  [media] guest dlopen(\"%s\") -> stub handle "
                    "(OpenMAX AL is not implemented)\n", b);
    return (void *)g_oma_handle;
}

int kl_mediandk_is_handle(const void *h) {
    return h == (const void *)g_md_handle || h == (const void *)g_oma_handle;
}

void *kl_mediandk_sym(const char *name) {
    void *fn = kl_mediandk_lookup(name);
    // dlsym semantics differ from the ELF-import door's: a miss here becomes a
    // named stub, because the guest is asking at runtime and deserves to fail
    // where it CALLS rather than where it looked up (CLAUDE.md, "lookups are
    // measurements, calls are assertions").
    return fn ? fn : kl_named_stub(name, (void *)kl_unresolved_named);
}
