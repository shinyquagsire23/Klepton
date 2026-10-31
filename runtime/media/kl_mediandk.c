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
#include <time.h>

#include "klepton.h"
#include "kl_ndk.h"
#include "kl_vtdec.h"
#include "kl_avdec.h"
#include "kl_vulkan.h"
#include "kl_glfb.h"
#include "kl_driver.h"
#include "kl_ovrp.h"

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
       AMEDIA_ERROR_INVALID_PARAMETER = -10004,
       AMEDIA_ERROR_INVALID_OPERATION = -10005 };
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
    // and in its own std::string temporaries.
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
// own default. Writing a zero on the false path hands back a value the caller
// never asked for, indistinguishable from a measurement.
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
static const char *g_key_rotation        = "rotation-degrees";
static const char *g_key_encoder_delay   = "encoder-delay";
static const char *g_key_slice_height    = "slice-height";

// ---------------------------------------------------------------------------
// AMediaExtractor — the DEMUXER, and the one part of this file that refuses.
//
// Unity's VideoPlayer on Android is extractor + codec: AMediaExtractor pulls
// compressed samples out of a container and hands them to AMediaCodec, which
// this file already serves over VideoToolbox. The codec half exists because
// Steam Link needed it and it arrives an elementary stream, already
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
    kl_avdemux *dm;         // the demuxer, when a container was opened (else NULL)
    // Which tracks the guest selected: bit 0 the picture, bit 1 the sound.
    // Until it selects any, the picture alone — what this extractor always did.
    unsigned sel;
    int      sel_set;
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
    if (x->dm) { kl_avdemux_close(x->dm); x->dm = NULL; }
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

// KL_MEDIA_EXTRACTOR=0: refuse every extractor source politely, so a guest whose
// video pipeline wedges a load (zix: a Unity VideoPlayer pump thread spinning in
// AMediaExtractor_getSampleTrackIndex/advance while the hub scene never
// activates) errors its VideoPlayer and proceeds without the video. Diagnostic
// A/B first, per-target default only if it proves out — default ON (1) serves
// video exactly as before for everyone.
//
// ...and the per-guest default that did prove out, on the other axis: a VULKAN
// guest. Unity's VideoPlayer takes decoded frames as AHardwareBuffers and, under
// Vulkan, imports them with vkGetAndroidHardwareBufferPropertiesANDROID /
// VK_ANDROID_external_memory_android_hardware_buffer. kl_vulkan answers both
// with the no-op stub it gives anything MoltenVK lacks, so a clip that demuxed
// and decoded perfectly would then be imported from properties nobody wrote.
// Until that import exists the honest answer is the refusal the guest already
// handles — the VideoPlayer errors and the scene carries on without the clip
// (Arkham Shadow's tutorial panels). KL_MEDIA_EXTRACTOR=1 forces the real path
// for whoever builds the import.
//
// kl_mediandk_set_video_default(1) is the same override as a stored setting
// (the frontend's "Videos" toggle): it makes the default ON regardless of the
// guest, for the run whose log is going to say what the guest does next.
static int g_video_default = -1;
void kl_mediandk_set_video_default(int on) { g_video_default = on; }
static int klm_extractor_enabled(void) {
    static int on = -1;
    if (on < 0) {
        int vk = kl_vulkan_guest_active();
        const char *target = kl_driver_target_name();
        // Shave & Stuff's rotating promo panel is the only video used before
        // play. On this GLES path Unity submits thousands of decoded frames but
        // consumes only a handful, leaving the panel visibly torn and adding
        // needless decode work. Refuse that optional clip by default for this
        // title; the "Videos" setting and KL_MEDIA_EXTRACTOR=1 still turn it on.
        int shave = target && !strcmp(target, "x7532973430077887");
        // Mission: ISS opens with a 360 intro clip and its IntroScene waits for
        // the clip's end before the main menu loads: refused, the player sees
        // black for as long as he waits. Under Vulkan this Unity build uses
        // BUFFER MODE (configure with no surface, NV12 bytes uploaded by the
        // guest itself — klm_linearize below), not the AHardwareBuffer import
        // the refusal above is about, so the clip plays as it does under GLES.
        int iss = target && !strcmp(target, "x2094303753986147");
        // Gundam: Silver Phantom waits for its in-world monitor's clip.
        // Refusing the extractor leaves AndroidVideoMedia retrying -10002
        // indefinitely. Let this title reach the decoder so its video path
        // can run; keep the Vulkan refusal for all other unmeasured guests.
        int gundam = target && !strcmp(target, "x6811683248909506");
        int default_on = g_video_default > 0 ? 1 : shave ? 0 : (iss || gundam) ? 1 : !vk;
        on = kl_env_on("KL_MEDIA_EXTRACTOR", default_on);
        if (on && vk)
            fprintf(stderr, "  [media] video is enabled for this Vulkan "
                            "guest (the target's default or the setting)\n");
        if (!on && vk)
            fprintf(stderr, "  [media] video is refused for this Vulkan guest: "
                            "decoded frames cannot be imported into its VkDevice "
                            "yet (KL_MEDIA_EXTRACTOR=1 tries anyway)\n");
    }
    return on;
}

static int klm_AMediaExtractor_setDataSource(void *ex, const char *location) {
    klm_extractor *x = klm_ex(ex);
    if (!x) return AMEDIA_ERROR_INVALID_OBJECT;
    if (!klm_extractor_enabled()) return klm_extractor_refuse(x, location);
    if (location) {
        x->dm = kl_avdemux_open(location, 0, 0);
        if (x->dm) {
            free(x->source); x->source = strdup(location);
            fprintf(stderr, "  [media] AMediaExtractor: demuxing \"%s\"\n", location);
            return AMEDIA_OK;
        }
    }
    return klm_extractor_refuse(x, location);
}

static int klm_AMediaExtractor_setDataSourceFd(void *ex, int fd, int64_t offset, int64_t length) {
    klm_extractor *x = klm_ex(ex);
    if (!x) return AMEDIA_ERROR_INVALID_OBJECT;
    if (!klm_extractor_enabled()) return klm_extractor_refuse(x, NULL);
    // The fd's own path — the video lives at [offset, offset+length) inside it
    // (a subfile of the OBB, usually). kl_avdemux slices that range out and
    // demuxes it; the codec half (AMediaCodec -> kl_vtdec) decodes the Annex-B.
    char path[1024];
    int named = fd >= 0 && fcntl(fd, F_GETPATH, path) == 0;
    if (named) {
        x->dm = kl_avdemux_open(path, (long long)offset, (long long)length);
        if (x->dm) {
            free(x->source); x->source = strdup(path);
            fprintf(stderr, "  [media] AMediaExtractor: demuxing \"%s\" [%lld+%lld]\n",
                    path, (long long)offset, (long long)length);
            return AMEDIA_OK;
        }
    }
    return klm_extractor_refuse(x, named ? path : NULL);
}

// ---------------------------------------------------------------------------
// AMediaDataSource — a container the GUEST reads for us (API 28).
//
// Unity's VideoPlayer takes this route instead of setDataSourceFd whenever the
// movie is not a plain span of a file it can hand over as (fd, offset, length):
// it builds a data source, installs readAt/getSize/close callbacks over its own
// file abstraction, and gives it to the extractor. Arkham Shadow's first
// in-game tutorial clip (the glide prompt) is the first title here to do it,
// and all seven symbols were WEAK imports left NULL — so the call to
// AMediaDataSource_new was a branch to address 0, reported by visionOS as
// "CODESIGNING, Invalid Page" with nothing in our own log.
//
// kl_avdemux reads files, so the source is drained into one: TMPDIR, the same
// cache slice_out() uses and for the same reasons, keyed on the size the guest
// reports. The callbacks are guest code, called here on the guest's own thread
// inside setDataSourceCustom — which is where Android's MediaExtractor does its
// first reads too, so nothing about threading is new to the guest.
#define KLM_DATASOURCE_MAGIC 0x4b4c4d44u   /* 'KLMD' */
typedef ssize_t (*klm_ds_read_at)(void *userdata, off_t offset, void *buffer, size_t size);
typedef ssize_t (*klm_ds_get_size)(void *userdata);
typedef void    (*klm_ds_close)(void *userdata);
typedef struct {
    uint32_t        magic;
    void           *userdata;
    klm_ds_read_at  read_at;
    klm_ds_get_size get_size;
    klm_ds_close    close;
} klm_datasource;

static klm_datasource *klm_ds(void *p) {
    klm_datasource *d = p;
    return (d && d->magic == KLM_DATASOURCE_MAGIC) ? d : NULL;
}
static void *klm_AMediaDataSource_new(void) {
    klm_datasource *d = calloc(1, sizeof *d);
    if (d) d->magic = KLM_DATASOURCE_MAGIC;
    return d;
}
static void klm_AMediaDataSource_delete(void *p) {
    klm_datasource *d = klm_ds(p);
    if (!d) return;
    d->magic = 0;
    free(d);
}
static void klm_AMediaDataSource_setUserdata(void *p, void *userdata) {
    klm_datasource *d = klm_ds(p);
    if (d) d->userdata = userdata;
}
static void klm_AMediaDataSource_setReadAt(void *p, klm_ds_read_at fn) {
    klm_datasource *d = klm_ds(p);
    if (d) d->read_at = fn;
}
static void klm_AMediaDataSource_setGetSize(void *p, klm_ds_get_size fn) {
    klm_datasource *d = klm_ds(p);
    if (d) d->get_size = fn;
}
static void klm_AMediaDataSource_setClose(void *p, klm_ds_close fn) {
    klm_datasource *d = klm_ds(p);
    if (d) d->close = fn;
}

static int klm_AMediaExtractor_setDataSourceCustom(void *ex, void *src) {
    klm_extractor *x = klm_ex(ex);
    klm_datasource *d = klm_ds(src);
    if (!x) return AMEDIA_ERROR_INVALID_OBJECT;
    if (!d || !d->read_at || !d->get_size) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (!klm_extractor_enabled()) return klm_extractor_refuse(x, "(custom data source)");

    long long size = (long long)d->get_size(d->userdata);
    // 2 GiB is far past any clip a title streams this way, and a negative or
    // absurd size is the guest's "unknown length" — a stream, which a file
    // demuxer cannot serve.
    if (size <= 0 || size > (2LL << 30)) {
        fprintf(stderr, "  [media] AMediaExtractor_setDataSourceCustom: the source "
                        "reports %lld bytes — refused\n", size);
        return klm_extractor_refuse(x, "(custom data source)");
    }
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    static unsigned seq;
    char path[1024];
    snprintf(path, sizeof path, "%s/klepton-media-ds-%d-%u-%lld.mp4", tmp,
             (int)getpid(), __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED), size);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return klm_extractor_refuse(x, "(custom data source)");
    enum { CHUNK = 1 << 20 };
    unsigned char *buf = malloc(CHUNK);
    long long done = 0;
    int ok = buf != NULL;
    while (ok && done < size) {
        size_t want = size - done < CHUNK ? (size_t)(size - done) : CHUNK;
        ssize_t got = d->read_at(d->userdata, (off_t)done, buf, want);
        // 0 or negative before the stated size is a truncated container, and
        // has to fail here: AVFoundation would refuse it later for reasons that
        // name nothing.
        if (got <= 0 || (size_t)got > want) { ok = 0; break; }
        if (write(fd, buf, (size_t)got) != got) { ok = 0; break; }
        done += got;
    }
    free(buf);
    close(fd);
    if (ok) x->dm = kl_avdemux_open(path, 0, 0);
    // The demuxer holds the asset by URL and reads it lazily, so the file stays
    // until the process's TMPDIR is reclaimed; a failed spool is removed now.
    if (!ok || !x->dm) {
        unlink(path);
        fprintf(stderr, "  [media] AMediaExtractor_setDataSourceCustom: %s after "
                        "%lld of %lld bytes\n",
                ok ? "the demuxer refused the container" : "the source stopped",
                done, size);
        return klm_extractor_refuse(x, "(custom data source)");
    }
    free(x->source); x->source = strdup(path);
    fprintf(stderr, "  [media] AMediaExtractor: demuxing a custom data source, "
                    "%lld bytes spooled to \"%s\"\n", size, path);
    return AMEDIA_OK;
}

// Everything past the refusal describes an extractor with nothing in it. These
// are the values the NDK defines for "no more samples" / "no such track", not
// invented ones, so a guest that ignored the setDataSource status still walks a
// consistent empty stream rather than reading uninitialised memory.
// The clip's sound as a second track (track 1), "audio/raw": AVFoundation has
// already decoded it (kl_avdemux_audio_*), so the codec behind it only hands
// the PCM through (klm_pcm_*). Before this every clip was silent — the
// extractor had one track. KL_MEDIA_SOUND=0|1; on by default for LES MILLS
// BODYCOMBAT (x4015163475201433), whose workout clips carry their trainer's
// voice (on device, 2026-10-15), and off elsewhere until a title is seen with it.
static int klm_sound_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_MEDIA_SOUND", t && !strcmp(t, "x4015163475201433"));
    }
    return on;
}
static int klm_ex_has_sound(klm_extractor *x) {
    return x && x->dm && klm_sound_enabled() && kl_avdemux_audio_info(x->dm, NULL, NULL);
}
static size_t klm_AMediaExtractor_getTrackCount(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm) return 0;
    return klm_ex_has_sound(x) ? 2 : 1;
}
static void *klm_AMediaExtractor_getTrackFormat(void *ex, size_t i) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm) return NULL;
    if (i == 1 && klm_ex_has_sound(x)) {
        int rate = 0, ch = 0; long long dur = 0;
        kl_avdemux_audio_info(x->dm, &rate, &ch);
        kl_avdemux_info(x->dm, NULL, NULL, NULL, &dur, NULL);
        klm_format *f = klm_AMediaFormat_new();
        if (!f) return NULL;
        klm_AMediaFormat_setString(f, g_key_mime, "audio/raw");
        klm_AMediaFormat_setInt32(f, g_key_sample_rate, rate);
        klm_AMediaFormat_setInt32(f, g_key_channel_count, ch);
        klm_AMediaFormat_setInt32(f, g_key_duration, (int32_t)dur);
        klm_AMediaFormat_setInt32(f, "pcm-encoding", 2);          // ENCODING_PCM_16BIT
        klm_AMediaFormat_setInt32(f, "max-input-size", 1024 * ch * 2);
        return f;
    }
    if (i != 0) return NULL;
    int w = 0, h = 0; const char *mime = NULL; long long dur = 0; float fps = 0;
    kl_avdemux_info(x->dm, &w, &h, &mime, &dur, &fps);
    klm_format *f = klm_AMediaFormat_new();
    if (!f) return NULL;
    klm_AMediaFormat_setString(f, g_key_mime, mime ? mime : "video/avc");
    klm_AMediaFormat_setInt32(f, g_key_width, w);
    klm_AMediaFormat_setInt32(f, g_key_height, h);
    klm_AMediaFormat_setInt32(f, g_key_duration, (int32_t)dur);
    // Frame rate: without it Unity's AndroidVideoMedia::GetFormatInfo cannot
    // read a rate from the format, falls back to timing the first two samples,
    // gets a zero delta on our probe path, logs "Unable to detect video frame
    // rate", and tears the VideoPlayer down before real playback ever starts —
    // which is why the world was black. nominalFrameRate gives it directly.
    if (fps > 0)
        klm_AMediaFormat_setInt32(f, g_key_frame_rate, (int32_t)(fps + 0.5f));
    // Unity sizes its codec input buffers from max-input-size; without it the
    // allocation is 0/garbage and the guest null-derefs in its allocator right
    // after opening the video. A keyframe (with the parameter sets we inject) is
    // well under this bound.
    if (w > 0 && h > 0)
        klm_AMediaFormat_setInt32(f, "max-input-size", w * h);
    return f;
}
static int klm_AMediaExtractor_selectTrack(void *ex, size_t i) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm || i >= klm_AMediaExtractor_getTrackCount(ex))
        return AMEDIA_ERROR_INVALID_PARAMETER;
    if (!x->sel_set) { x->sel = 0; x->sel_set = 1; }
    x->sel |= 1u << i;
    return AMEDIA_OK;
}
// Which selected track holds the next sample in time — Android's extractor
// interleaves its selected tracks by timestamp. -1 when all are at their end.
static int klm_ex_current(klm_extractor *x) {
    unsigned sel = x->sel_set ? x->sel : 1u;
    long long tv = (sel & 1u) ? kl_avdemux_sample_time_us(x->dm) : -1;
    long long ta = (sel & 2u) ? kl_avdemux_audio_time_us(x->dm) : -1;
    if (tv < 0 && ta < 0) return -1;
    if (tv < 0) return 1;
    if (ta < 0) return 0;
    return ta < tv ? 1 : 0;
}
static ssize_t klm_AMediaExtractor_readSampleData(void *ex, uint8_t *buf, size_t cap) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm) return -1;
    int t = klm_ex_current(x);
    if (t == 1) return (ssize_t)kl_avdemux_audio_read(x->dm, buf, (unsigned long)cap);
    if (t == 0) return (ssize_t)kl_avdemux_read(x->dm, buf, (unsigned long)cap);
    return -1;
}
static int klm_AMediaExtractor_getSampleTrackIndex(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm) return -1;
    return klm_ex_current(x);
}
static int64_t klm_AMediaExtractor_getSampleTime(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm) return -1;
    int t = klm_ex_current(x);
    if (t == 1) return (int64_t)kl_avdemux_audio_time_us(x->dm);
    if (t == 0) return (int64_t)kl_avdemux_sample_time_us(x->dm);
    return -1;
}
static bool klm_AMediaExtractor_advance(void *ex) {
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm) return false;
    int t = klm_ex_current(x);
    if (t == 1) kl_avdemux_audio_advance(x->dm);
    else if (t == 0) kl_avdemux_advance(x->dm);
    else return false;
    return klm_ex_current(x) >= 0;
}
static int klm_AMediaExtractor_seekTo(void *ex, int64_t us, int mode) {
    (void)mode;
    klm_extractor *x = klm_ex(ex);
    if (!x || !x->dm) return AMEDIA_ERROR_UNSUPPORTED;
    unsigned sel = x->sel_set ? x->sel : 1u;
    int ok = 1;
    if (sel & 1u) ok &= kl_avdemux_seek_us(x->dm, (long long)us) != 0;
    if (sel & 2u) kl_avdemux_audio_seek_us(x->dm, (long long)us);
    return ok ? AMEDIA_OK : AMEDIA_ERROR_UNSUPPORTED;
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
// stream is healthiest. Sized above the request, and the request is reported.
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
    unsigned texture;
    unsigned n_latched, n_upload_failed, n_empty_updates;
    int64_t first_pts_ns;
    int is_surface_texture;
    void *latched_image;
    int64_t timestamp_ns;

    pthread_t dispatch;
    int       running;
    int       pending;          // listener calls owed to the guest
    // Per reader, where KLM_FIRST is per run: a game whose FIRST clip plays
    // and a later one stays empty (LES MILLS BODYCOMBAT's level-start clip,
    // 2026-10-15) said nothing at all about the later one.
    int       said_pub, said_latch, said_fail;
    int       serial;           // which reader of the run this is
    uint64_t  beat_ns;          // last updateTexImage heartbeat
    // Evenness of the picture: wall time between two latched frames against
    // the pts step between them. A late latch is one that came more than a
    // half frame after its pts said it should; reset with every heartbeat.
    uint64_t  latch_ns;
    int64_t   latch_pts_ns;
    // Images taken off the texture but kept alive a few latches longer
    // (klm_retire): the GPU may still be drawing a guest frame that samples one.
    void     *retired[8];
    int       n_retired;
    unsigned  n_backward;       // latched pts went back (reset per heartbeat)
    unsigned  n_late;
    uint64_t  worst_gap_ns;
} klm_reader;

#define KLM_READER_MAGIC 0x4b4c5244u   /* 'KLRD' */
static void klm_retire_all(klm_reader *r);

static klm_reader *g_reader;        // for the report; this guest makes one
// Lifetime totals. The per-object counters die with the object, and the run
// where "did anything decode?" matters most is the CLEAN one — where the app
// shuts down and deletes both the codec and the reader before anything gets
// to report. These outlive that: an empty media report otherwise reads as "the
// decoder was never fed" when it only means "teardown got there first".
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
// a real thread" rule, in a new subsystem. One thread per reader,
// woken by the queue.
static void *reader_dispatch(void *arg) {
    klm_reader *r = arg;
    // Guest code runs on this thread, so the per-thread TLS and veneer setup
    // has to happen before the first call into it.
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
    static int serial;
    r->serial = ++serial;
    fprintf(stderr, "  [media] AImageReader #%d %dx%d format 0x%x usage 0x%llx, "
                    "%d images\n", r->serial, w, h, format, (unsigned long long)usage,
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
    if (r->is_surface_texture)
        fprintf(stderr,"  [media] SurfaceTexture summary (reader #%d): queued=%u latched=%u skipped=%u upload_failed=%u empty_updates=%u first_pts_ns=%lld last_pts_ns=%lld\n",
            r->serial,r->n_queued,r->n_latched,r->n_dropped,r->n_upload_failed,r->n_empty_updates,
            (long long)r->first_pts_ns,(long long)r->timestamp_ns);
    if (r->latched_image) kl_glfb_image_destroy(r->latched_image);
    klm_retire_all(r);
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
    if (!r->said_pub) {
        r->said_pub = 1;
        fprintf(stderr, "  [media] reader #%d: first frame %zux%zu published (listener %s)\n",
                r->serial, CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb),
                r->have_listener ? "set" : "NOT set");
    }
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->lock);
}

// A frame taken off the texture is not destroyed at once. Destroying it gives
// its pixel buffer back to the decoder's pool, and VideoToolbox writes a LATER
// frame into it straight away — while guest frames already submitted (the
// guest renders up to three ahead) still sample it on the GPU. What they then
// show is a frame from further on in the clip, for one refresh: the picture
// jumps forward and back. LES MILLS BODYCOMBAT's trainer clips did exactly
// that (on device, 2026-10-15). So the last few images are kept alive. Any
// guest on this path renders ahead the same way, but only this one has been
// seen with it, so the others keep the immediate destroy until one is.
// KL_VIDEO_RETIRE=<n> (0..8; default 6 for LES MILLS BODYCOMBAT
// (x4015163475201433), else 0, which destroys at once as before).
static int klm_retire_depth(void) {
    static int n = -1;
    if (n < 0) {
        const char *t = kl_driver_target_name();
        n = (int)kl_env_int("KL_VIDEO_RETIRE", t && !strcmp(t, "x4015163475201433") ? 6 : 0);
        if (n < 0) n = 0;
        if (n > 8) n = 8;
    }
    return n;
}
static void klm_retire(klm_reader *r, void *image) {
    if (!image) return;
    int depth = klm_retire_depth();
    if (!depth) { kl_glfb_image_destroy(image); return; }
    if (r->n_retired == depth) {
        kl_glfb_image_destroy(r->retired[0]);
        memmove(r->retired, r->retired + 1, (size_t)(depth - 1) * sizeof r->retired[0]);
        r->n_retired--;
    }
    r->retired[r->n_retired++] = image;
}
static void klm_retire_all(klm_reader *r) {
    for (int i = 0; i < r->n_retired; i++) kl_glfb_image_destroy(r->retired[i]);
    r->n_retired = 0;
}

// SurfaceTexture's producer window is a real reader window, so codec release
// reaches the existing asynchronous notification and latest-frame queue.
void *kl_mediandk_texture_new(unsigned texture) {
    void *out = NULL;
    if (klm_AImageReader_newWithUsage(1, 1, 0x22, 0, KLM_READER_MAX, &out)) return NULL;
    ((klm_reader *)out)->texture = texture;
    ((klm_reader *)out)->is_surface_texture = 1;
    return out;
}
void *kl_mediandk_texture_window(void *reader) {
    void *window = NULL;
    return klm_AImageReader_getWindow(reader, &window) == AMEDIA_OK ? window : NULL;
}
void kl_mediandk_texture_listener(void *reader, void *context, void (*callback)(void *, void *)) {
    klm_listener l = {context, callback};
    klm_AImageReader_setImageListener(reader, callback ? &l : NULL);
}
int kl_mediandk_texture_update(void *reader) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC) return 0;
    // The game's side of klm_codec_beat: that it still asks for frames at all.
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        if (!r->beat_ns || now - r->beat_ns >= 2000000000ull) {
            r->beat_ns = now;
            fprintf(stderr, "  [media] reader #%d updateTexImage: queued=%u latched=%u "
                            "empty_updates=%u upload_failed=%u | late latches %u, "
                            "worst gap %.1f ms, went back %u\n", r->serial,
                    r->n_queued, r->n_latched, r->n_empty_updates, r->n_upload_failed,
                    r->n_late, (double)r->worst_gap_ns / 1e6, r->n_backward);
            r->n_late = 0; r->worst_gap_ns = 0; r->n_backward = 0;
        }
    }
    if (!r->texture) {
        if (!r->said_fail) {
            r->said_fail = 1;
            fprintf(stderr, "  [media] reader #%d: updateTexImage with no texture attached\n",
                    r->serial);
        }
        return 0;
    }
    void *image = NULL;
    if (klm_AImageReader_acquireLatestImage(r, &image) != AMEDIA_OK) { r->n_empty_updates++; return 0; }
    klm_image *im = image;
    void *next = kl_glfb_image_from_pixels(im->pixels, NULL, NULL);
    int64_t timestamp = im->pts_us * 1000;
    klm_AImage_delete(im);
    if (!next || !kl_glfb_image_bind_texture(next, r->texture)) {
        r->n_upload_failed++;
        if (!r->said_fail) {
            r->said_fail = 1;
            fprintf(stderr, "  [media] reader #%d: a decoded frame could not be %s texture %u\n",
                    r->serial, next ? "bound to" : "made into an image for", r->texture);
        }
        if (next) kl_glfb_image_destroy(next);
        return 0;
    }
    klm_retire(r, r->latched_image);
    r->latched_image = next;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        if (r->latch_ns && timestamp < r->latch_pts_ns) r->n_backward++;
        if (r->latch_ns && timestamp > r->latch_pts_ns) {
            uint64_t gap = now - r->latch_ns;
            int64_t step = timestamp - r->latch_pts_ns;
            if (gap > r->worst_gap_ns) r->worst_gap_ns = gap;
            if ((int64_t)gap > step + step / 2) r->n_late++;
        }
        r->latch_ns = now;
        r->latch_pts_ns = timestamp;
    }
    r->timestamp_ns = timestamp;
    if (!r->n_latched) r->first_pts_ns=timestamp;
    r->n_latched++;
    KLM_FIRST(said_texture, "SurfaceTexture latched decoded frame into texture %u\n", r->texture);
    if (!r->said_latch) {
        r->said_latch = 1;
        fprintf(stderr, "  [media] reader #%d: first frame latched into texture %u\n",
                r->serial, r->texture);
    }
    return 1;
}
long long kl_mediandk_texture_timestamp(void *reader) {
    klm_reader *r = reader;
    return r && r->magic == KLM_READER_MAGIC ? r->timestamp_ns : 0;
}
void kl_mediandk_texture_size(void *reader, int width, int height) {
    klm_reader *r = reader;
    if (r && r->magic == KLM_READER_MAGIC && width > 0 && height > 0)
        kl_ndk_window_set_size(r->window, width, height);
}
void kl_mediandk_texture_attach(void *reader, unsigned texture) {
    klm_reader *r = reader;
    if (r && r->magic == KLM_READER_MAGIC) r->texture = texture;
}
void kl_mediandk_texture_detach(void *reader) {
    klm_reader *r = reader;
    if (!r || r->magic != KLM_READER_MAGIC) return;
    if (r->latched_image) kl_glfb_image_destroy(r->latched_image);
    klm_retire_all(r);
    r->latched_image = NULL;
    r->texture = 0;
}
void kl_mediandk_texture_delete(void *reader) { klm_AImageReader_delete(reader); }

// A decoder handoff gate with real CVPixelBuffers and the real reader queue.
static void texture_test_callback(void *ctx, void *reader) {
    (void)reader;
    __atomic_add_fetch((int *)ctx, 1, __ATOMIC_SEQ_CST);
}
int kl_mediandk_texture_selftest(FILE *f) {
    klm_reader *r = kl_mediandk_texture_new(17);
    if (!r) return 0;
    int called = 0;
    kl_mediandk_texture_listener(r, &called, texture_test_callback);
    CVPixelBufferRef pb = NULL;
    if (CVPixelBufferCreate(NULL, 16, 8, kCVPixelFormatType_32BGRA, NULL, &pb)) {
        kl_mediandk_texture_delete(r); return 0;
    }
    klm_reader_publish(r, pb, 12);
    klm_reader_publish(r, pb, 34);
    CVPixelBufferRelease(pb);
    for (int i = 0; i < 100 && __atomic_load_n(&called, __ATOMIC_SEQ_CST) < 2; i++) usleep(1000);
    void *im = NULL;
    int w = 0, h = 0;
    kl_ndk_window_size(r->window, &w, &h);
    int ok = kl_ndk_window_owner(r->window) == r && w == 16 && h == 8 &&
        __atomic_load_n(&called, __ATOMIC_SEQ_CST) == 2 &&
        klm_AImageReader_acquireLatestImage(r, &im) == AMEDIA_OK;
    if (im) { ok &= ((klm_image *)im)->pts_us == 34; klm_AImage_delete(im); }
    kl_mediandk_texture_delete(r);
    fprintf(f, "%s: SurfaceTexture window, async notification, decoded frame and latest timestamp\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// AMediaCodec

#define KLM_IN_BUFFERS  8
#define KLM_IN_CAP      (2u << 20)      // per input buffer, at least; a 4K keyframe fits
#define KLM_IN_CAP_MAX  (64u << 20)     // ...and at most, whatever max-input-size says
#define KLM_OUT_SLOTS   16

typedef struct {
    uint32_t magic;
    char     mime[32];
    kl_vtdec *dec;
    klm_reader *surface;
    int      started;

    pthread_mutex_t lock;
    int playback_backpressure;
    unsigned n_input_waits;
    struct { uint8_t *data; size_t cap; int in_use; } in[KLM_IN_BUFFERS];
    size_t   in_cap;    // what getInputBuffer hands out: KLM_IN_CAP or max-input-size
    struct { void *pb; int64_t pts_us; int in_use;
             uint8_t *lin; size_t lin_cap, lin_size; } out[KLM_OUT_SLOTS];
    // Buffer mode (no output surface): the layout every `lin` above is in, and
    // what getOutputFormat reports — see klm_linearize.
    int      lin_full_range;

    // The decoded size, learned from the first frame rather than from
    // configure(): the bitstream's own parameter sets are more authoritative
    // than anything the guest can say, and until a frame exists there is
    // nothing to report. AMediaCodec_getOutputFormat reads these.
    int      out_w, out_h;

    unsigned n_in, n_out, n_rendered;
    uint64_t beat_ns;           // last klm_codec_beat line

    // "audio/raw": a clip's sound, already PCM (see the extractor), so this
    // codec hands each input buffer through as an output buffer, in order.
    int      pcm, pcm_rate, pcm_ch;
    int      pcm_fmt_said;      // INFO_OUTPUT_FORMAT_CHANGED handed out

    // Even cadence (klm_even_cadence): the clip's frame period as measured, the
    // grid slot of the last frame handed out, and how many were left out.
    int64_t  ec_last_src_pts, ec_period_us, ec_last_slot;
    int      ec_have_slot;
    unsigned ec_dropped;
    struct { uint8_t *buf; size_t cap, len; int64_t pts_us; uint32_t flags;
             int state; unsigned seq; } pq[KLM_OUT_SLOTS];   // state: 0 free, 1 ready, 2 out
    unsigned pq_seq;
} klm_codec;

#define KLM_CODEC_MAGIC 0x4b4c4344u    /* 'KLCD' */

static uint64_t klm_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Every two seconds while a surface-configured codec is driven: what went in,
// what came out, what was rendered to its reader, and what the game took from
// that reader. A clip that shows its first frame and then stands still
// (LES MILLS BODYCOMBAT's level-start clip, 2026-10-15) is one of these
// numbers stopping, and the summaries at delete never come for a player the
// game keeps. Silence from here means the guest stopped calling the codec.
static void klm_codec_beat(klm_codec *c, const char *where) {
    if (!c || !c->surface) return;
    uint64_t now = klm_now_ns();
    if (c->beat_ns && now - c->beat_ns < 2000000000ull) return;
    c->beat_ns = now;
    klm_reader *r = c->surface;
    fprintf(stderr, "  [media] codec %dx%d -> reader #%d (%s): in=%u out=%u rendered=%u "
                    "input_waits=%u | reader queued=%u acquired=%u latched=%u dropped=%u "
                    "empty_updates=%u upload_failed=%u listener_owed=%d\n",
            c->out_w, c->out_h, r->serial, where, c->n_in, c->n_out, c->n_rendered,
            c->n_input_waits, r->n_queued, r->n_acquired, r->n_latched, r->n_dropped,
            r->n_empty_updates, r->n_upload_failed, r->pending);
}
// MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar — NV12, the
// one layout buffer mode produces (klm_linearize).
#define KLM_COLOR_FormatYUV420SemiPlanar 21

static klm_codec *g_codec;

// ---- the pass-through codec behind "audio/raw" -----------------------------
static void klm_pcm_reset(klm_codec *c) {
    for (int i = 0; i < KLM_OUT_SLOTS; i++) { c->pq[i].state = 0; c->pq[i].len = 0; }
    for (int i = 0; i < KLM_IN_BUFFERS; i++) c->in[i].in_use = 0;
}
static int klm_pcm_free_slot(klm_codec *c) {
    for (int i = 0; i < KLM_OUT_SLOTS; i++) if (!c->pq[i].state) return i;
    return -1;
}
static ssize_t klm_pcm_dequeue_input(klm_codec *c, int64_t timeoutUs) {
    for (int64_t waited = 0;; waited += 1000) {
        pthread_mutex_lock(&c->lock);
        if (klm_pcm_free_slot(c) >= 0)
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
    }
}
static int klm_pcm_queue_input(klm_codec *c, size_t idx, off_t offset, size_t size,
                               uint64_t time_us, uint32_t flags) {
    pthread_mutex_lock(&c->lock);
    uint8_t *p = c->in[idx].data;
    int o = klm_pcm_free_slot(c);
    int result = AMEDIA_OK;
    if (o < 0) result = AMEDIA_ERROR_INVALID_OPERATION;
    else if (size && (!p || offset < 0 || size > c->in[idx].cap || (size_t)offset > c->in[idx].cap - size))
        result = AMEDIA_ERROR_INVALID_PARAMETER;
    else {
        if (c->pq[o].cap < size) {
            uint8_t *n = realloc(c->pq[o].buf, size);
            if (!n) result = AMEDIA_ERROR_UNKNOWN;
            else { c->pq[o].buf = n; c->pq[o].cap = size; }
        }
        if (result == AMEDIA_OK) {
            if (size) memcpy(c->pq[o].buf, p + offset, size);
            c->pq[o].len = size;
            c->pq[o].pts_us = (int64_t)time_us;
            c->pq[o].flags = flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
            c->pq[o].seq = ++c->pq_seq;
            c->pq[o].state = 1;
            c->n_in++;
        }
    }
    c->in[idx].in_use = 0;
    pthread_mutex_unlock(&c->lock);
    KLM_FIRST(said_pcm_in, "first sound buffer queued: %zu bytes, pts %llu us\n",
              size, (unsigned long long)time_us);
    return result;
}
static ssize_t klm_pcm_dequeue_output(klm_codec *c, AMediaCodecBufferInfo *info,
                                      int64_t timeoutUs) {
    for (int64_t waited = 0;; waited += 1000) {
        pthread_mutex_lock(&c->lock);
        int best = -1;
        for (int i = 0; i < KLM_OUT_SLOTS; i++)
            if (c->pq[i].state == 1 && (best < 0 || c->pq[i].seq < c->pq[best].seq)) best = i;
        // Android's decoder says what it decodes to before the first buffer,
        // and Unity's AndroidVideoMedia::GetFormatInfo waits for exactly that:
        // without it the track reads as "format not detected" and the clip
        // plays without its sound.
        if (best >= 0 && !c->pcm_fmt_said) {
            c->pcm_fmt_said = 1;
            pthread_mutex_unlock(&c->lock);
            return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
        }
        if (best >= 0) {
            c->pq[best].state = 2;
            c->n_out++;
            if (info) {
                info->offset = 0;
                info->size = (int32_t)c->pq[best].len;
                info->presentationTimeUs = c->pq[best].pts_us;
                info->flags = c->pq[best].flags;
            }
            pthread_mutex_unlock(&c->lock);
            return best;
        }
        pthread_mutex_unlock(&c->lock);
        if (timeoutUs == 0 || (timeoutUs > 0 && waited >= timeoutUs))
            return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        usleep(1000);
    }
}


static void *klm_AMediaCodec_createDecoderByType(const char *mime) {
    klm_codec *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->magic = KLM_CODEC_MAGIC;
    const char *target=kl_driver_target_name();
    // LES MILLS BODYCOMBAT's VideoPlayer is the same case: with no backpressure it
    // queued a whole clip at once (in=472) into a 16-frame decoder ring and got
    // two frames out — the first stayed on screen (on device, 2026-10-15).
    c->playback_backpressure=target && (!strcmp(target,"x2249318828413747") ||
                                        !strcmp(target,"x4015163475201433"));
    snprintf(c->mime, sizeof c->mime, "%s", mime ? mime : "");
    pthread_mutex_init(&c->lock, NULL);
    if (mime && !strcmp(mime, "audio/raw")) {
        c->pcm = 1;
        c->playback_backpressure = 0;
        fprintf(stderr, "  [media] AMediaCodec_createDecoderByType(\"audio/raw\") — "
                        "the clip's sound, passed through\n");
        g_ever_created = 1;
        return c;
    }
    c->dec = kl_vtdec_create(mime);
    // Presentation order out of the decoder (kl_vtdec_want_reorder).
    // KL_VTDEC_REORDER=0|1; on by default for LES MILLS BODYCOMBAT, whose
    // workout and tip clips carry B-frames and jumped back every few seconds
    // in decode order (on device, 2026-10-15).
    {
        static int reorder = -1;
        if (reorder < 0)
            reorder = kl_env_on("KL_VTDEC_REORDER", target && !strcmp(target, "x4015163475201433"));
        if (reorder) kl_vtdec_want_reorder(c->dec, 1);
    }
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
        //. Steam Link never passes one; refusing
        // rather than ignoring keeps it that way.
        fprintf(stderr, "  [media] AMediaCodec_configure with an AMediaCrypto — "
                        "refused\n");
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    const char *mime = klm_fmt_string((klm_format *)format, "mime");
    if (c->pcm) {
        int32_t v;
        if (klm_AMediaFormat_getInt32((void *)format, g_key_sample_rate, &v)) c->pcm_rate = v;
        if (klm_AMediaFormat_getInt32((void *)format, g_key_channel_count, &v)) c->pcm_ch = v;
        fprintf(stderr, "  [media] AMediaCodec_configure: audio/raw %d Hz, %d channel(s)\n",
                c->pcm_rate, c->pcm_ch);
        return AMEDIA_OK;
    }
    // The surface is the AImageReader's window; that is the only kind this
    // guest passes, and the owner pointer the window carries is what connects
    // the two objects back up.
    klm_reader *r = kl_ndk_window_owner(window);
    c->surface = r;
    // The guest copies a whole access unit into one input buffer and sizes its
    // own staging from the format's max-input-size (ours says width * height).
    // A buffer smaller than that refuses the big keyframes with
    // INVALID_PARAMETER; every frame up to the next keyframe then decodes
    // against a missing reference (VideoToolbox -12909) and the picture breaks
    // into blocks. Mission: ISS's 2880x2880 intro has keyframes past 2 MB.
    int32_t max_in = 0;
    if (klm_AMediaFormat_getInt32((void *)format, "max-input-size", &max_in) && max_in > 0) {
        size_t want = (size_t)max_in;
        if (want > KLM_IN_CAP_MAX) want = KLM_IN_CAP_MAX;
        pthread_mutex_lock(&c->lock);
        if (want > c->in_cap) c->in_cap = want;
        pthread_mutex_unlock(&c->lock);
    }
    // No surface at all is BUFFER MODE: the guest will read each frame's bytes
    // (klm_linearize), so the decoder has to produce something readable.
    if (!window) kl_vtdec_want_nv12(c->dec, 1);
    fprintf(stderr, "  [media] AMediaCodec_configure: mime=%s surface=%p flags=0x%x, "
                    "input buffers %zu bytes\n",
            mime ? mime : "(unset)", window, flags,
            c->in_cap > KLM_IN_CAP ? c->in_cap : (size_t)KLM_IN_CAP);
    if (window && !r)
        fprintf(stderr, "  [media]   ...that window is not an AImageReader's; "
                        "rendered frames will have nowhere to go\n");
    return AMEDIA_OK;
}

static int klm_AMediaCodec_flush(void *codec);

static int klm_AMediaCodec_start(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&c->lock); c->started = 1; pthread_mutex_unlock(&c->lock);
    return AMEDIA_OK;
}

static int klm_AMediaCodec_stop(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&c->lock); c->started = 0; pthread_mutex_unlock(&c->lock);
    if (c->playback_backpressure) klm_AMediaCodec_flush(c);
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
    if (c->pcm) {
        pthread_mutex_lock(&c->lock); klm_pcm_reset(c); pthread_mutex_unlock(&c->lock);
        return AMEDIA_OK;
    }
    pthread_mutex_lock(&c->lock);
    klm_codec_drop_outputs(c);
    for (int i = 0; i < KLM_IN_BUFFERS; i++) c->in[i].in_use = 0;
    c->ec_have_slot = 0; c->ec_last_src_pts = 0;
    pthread_mutex_unlock(&c->lock);
    if (c->playback_backpressure) kl_vtdec_flush_playback(c->dec);
    else kl_vtdec_flush(c->dec);
    return AMEDIA_OK;
}

static int klm_AMediaCodec_delete(void *codec) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&c->lock);
    klm_codec_drop_outputs(c);
    for (int i = 0; i < KLM_IN_BUFFERS; i++) free(c->in[i].data);
    for (int i = 0; i < KLM_OUT_SLOTS; i++) free(c->out[i].lin);
    for (int i = 0; i < KLM_OUT_SLOTS; i++) free(c->pq[i].buf);
    if (c->pcm)
        fprintf(stderr, "  [media] sound codec summary: %u buffers in, %u out\n",
                c->n_in, c->n_out);
    pthread_mutex_unlock(&c->lock);
    if(c->playback_backpressure) {
        unsigned in=0,out=0,dropped=0;kl_vtdec_stats(c->dec,&in,&out,&dropped,NULL,NULL);
        fprintf(stderr,"  [media] playback codec summary: submitted=%u decoded=%u decoder_dropped=%u dequeued=%u input_backpressure=%u even_cadence_dropped=%u\n",in,out,dropped,c->n_out,c->n_input_waits,c->ec_dropped);
    }
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
    if (c->pcm) return klm_pcm_dequeue_input(c, timeoutUs);
    int64_t waited = 0;
    for (;;) {
        pthread_mutex_lock(&c->lock);
        if (c->playback_backpressure && !c->started) {
            pthread_mutex_unlock(&c->lock); return AMEDIA_ERROR_INVALID_OPERATION;
        }
        unsigned occupied=0;
        if (c->playback_backpressure) {
            occupied=kl_vtdec_pending(c->dec);
            for(int i=0;i<KLM_IN_BUFFERS;i++)occupied+=c->in[i].in_use!=0;
            for(int i=0;i<KLM_OUT_SLOTS;i++)occupied+=c->out[i].in_use!=0;
        }
        if (occupied < 12) for (int i = 0; i < KLM_IN_BUFFERS; i++)
            if (!c->in[i].in_use) {
                c->in[i].in_use = 1;
                pthread_mutex_unlock(&c->lock);
                return i;
            }
        if (occupied >= 12) c->n_input_waits++;
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
    size_t want = c->in_cap > KLM_IN_CAP ? c->in_cap : KLM_IN_CAP;
    if (!c->in[idx].data || c->in[idx].cap < want) {
        free(c->in[idx].data);
        c->in[idx].data = malloc(want);
        c->in[idx].cap = c->in[idx].data ? want : 0;
    }
    uint8_t *p = c->in[idx].data;
    size_t cap = c->in[idx].cap;
    pthread_mutex_unlock(&c->lock);
    if (out_size) *out_size = p ? cap : 0;
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
    if (out_size) *out_size = 0;
    if (!c || c->magic != KLM_CODEC_MAGIC) return NULL;
    if (c->pcm) {
        if (idx >= KLM_OUT_SLOTS) return NULL;
        pthread_mutex_lock(&c->lock);
        uint8_t *p = c->pq[idx].state == 2 ? c->pq[idx].buf : NULL;
        if (out_size && p) *out_size = c->pq[idx].cap;
        pthread_mutex_unlock(&c->lock);
        return p;
    }
    if (!c->surface) {
        // Buffer mode: the frame klm_linearize flattened at dequeue. Valid
        // until the guest releases the slot, as on Android.
        if (idx >= KLM_OUT_SLOTS) return NULL;
        pthread_mutex_lock(&c->lock);
        uint8_t *p = c->out[idx].in_use && c->out[idx].lin_size ? c->out[idx].lin : NULL;
        size_t n = p ? c->out[idx].lin_size : 0;
        pthread_mutex_unlock(&c->lock);
        if (out_size) *out_size = n;
        return p;
    }
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
    if (c->pcm) {
        klm_AMediaFormat_setInt32(f, g_key_sample_rate, c->pcm_rate);
        klm_AMediaFormat_setInt32(f, g_key_channel_count, c->pcm_ch);
        klm_AMediaFormat_setInt32(f, "pcm-encoding", 2);
        return f;
    }
    pthread_mutex_lock(&c->lock);
    int w = c->out_w, h = c->out_h;
    pthread_mutex_unlock(&c->lock);
    if (w && h) {
        klm_AMediaFormat_setInt32(f, g_key_width,  w);
        klm_AMediaFormat_setInt32(f, g_key_height, h);
    }
    if (!c->surface) {
        // Buffer mode DOES have a linear layout, and the guest cannot read the
        // bytes without it: NV12 at a row pitch of the width (klm_linearize).
        // Set before the first frame too — the layout is ours, not the
        // bitstream's, and a guest that asks early must not read format 0.
        klm_AMediaFormat_setInt32(f, g_key_color_format, KLM_COLOR_FormatYUV420SemiPlanar);
        if (w && h) {
            klm_AMediaFormat_setInt32(f, g_key_stride,       w);
            klm_AMediaFormat_setInt32(f, g_key_slice_height, h);
        }
        klm_AMediaFormat_setInt32(f, g_key_color_range,    c->lin_full_range ? 1 : 2);
        klm_AMediaFormat_setInt32(f, g_key_color_standard, 1);    // BT.709
        return f;
    }
    // Not the stride: the frames never pass through a linear buffer here (see
    // getOutputBuffer), so there is no row pitch to report and reporting one
    // would describe memory the guest cannot reach.
    return f;
}

static int klm_AMediaCodec_queueInputBuffer(void *codec, size_t idx, off_t offset,
                                            size_t size, uint64_t time_us,
                                            uint32_t flags) {
    klm_codec *c=codec;
    if(!c || c->magic!=KLM_CODEC_MAGIC || idx>=KLM_IN_BUFFERS) return AMEDIA_ERROR_INVALID_PARAMETER;
    if(c->pcm) return klm_pcm_queue_input(c,idx,offset,size,time_us,flags);
    pthread_mutex_lock(&c->lock);
    uint8_t *p=c->in[idx].data;
    c->n_in++;
    pthread_mutex_unlock(&c->lock);
    int result=AMEDIA_OK;
    if(!p) result=AMEDIA_ERROR_UNKNOWN;
    else if(offset<0 || size>c->in[idx].cap || (size_t)offset>c->in[idx].cap-size) {
        result=AMEDIA_ERROR_INVALID_PARAMETER;
        KLM_FIRST(said_in_big,"access unit of %zu bytes refused: the input buffer holds %zu "
                  "(max-input-size %zu)\n",size,c->in[idx].cap,c->in_cap);
    }
    else if(flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) kl_vtdec_finish(c->dec);
    else {
        if(!size) result=(flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) ? AMEDIA_OK : AMEDIA_ERROR_INVALID_PARAMETER;
        else {
            KLM_FIRST(said_in,"first access unit queued: %zu bytes, pts %llu us, flags 0x%x\n",size,(unsigned long long)time_us,flags);
            result=kl_vtdec_submit(c->dec,p+offset,size,(int64_t)time_us)==0 ? AMEDIA_OK : AMEDIA_ERROR_UNKNOWN;
        }
    }
    // Keep the reservation until submission is accounted, so another producer
    // cannot race through the bounded decoded/output-frame budget.
    pthread_mutex_lock(&c->lock);c->in[idx].in_use=0;pthread_mutex_unlock(&c->lock);
    return result;
}

// ---------------------------------------------------------------------------
// Buffer mode — a codec configured with NO surface.
//
// The other half of AMediaCodec, and the one Unity's VideoPlayer uses whenever
// it has no external-texture path for the renderer in use, which under Vulkan
// is always: it configures with a NULL window, dequeues, asks getOutputFormat
// for width / height / stride / slice-height / color-format, takes
// getOutputBuffer's bytes and uploads the planes itself (read out of
// libunity's AndroidVideoMedia conversion at 0xbe9308: Y at the base, chroma
// at base + sliceHeight * stride, and for COLOR_FormatYUV420SemiPlanar a
// two-channel plane of width/2 x height/2 with the SAME row pitch).
//
// So every decoded frame is flattened to exactly that: NV12, row pitch = width,
// slice height = height. VideoToolbox's native output for 8-bit H.264/HEVC is
// already bi-planar 4:2:0 with CbCr interleaved in that order, so this is two
// row-by-row copies that drop CoreVideo's row padding; a 10-bit or BGRA buffer
// is converted, because the guest was told NV12 either way.
//
// Measured before this existed (Arkham Shadow, its first tutorial clip): the
// demuxer opened the file, the decoder produced frames, and the guest logged
// "Error while trying to convert the video player's output format from 0"
// once per frame — format 0 being the key we did not set.
static size_t klm_linearize(CVPixelBufferRef pb, uint8_t **buf, size_t *cap, int *full) {
    size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
    size_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    size_t need = w * h + 2 * cw * ch + w;      // + one row of slack for odd widths
    if (*cap < need) {
        uint8_t *n = realloc(*buf, need);
        if (!n) return 0;
        *buf = n; *cap = need;
    }
    uint8_t *Y = *buf, *UV = *buf + w * h;
    OSType pf = CVPixelBufferGetPixelFormatType(pb);
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        memset(Y, 16, w * h);
        memset(UV, 128, w * ch);
        return w * h + w * ch;
    }
    int ok = 1;
    if (pf == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        pf == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        *full = pf == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
        const uint8_t *sy = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        const uint8_t *sc = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        size_t py = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
        size_t pc = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
        size_t hc = CVPixelBufferGetHeightOfPlane(pb, 1);
        if (!sy || !sc) ok = 0;
        for (size_t r = 0; ok && r < h; r++) memcpy(Y + r * w, sy + r * py, w);
        // The guest reads chroma rows at the luma pitch, 2 bytes per sample.
        for (size_t r = 0; ok && r < ch && r < hc; r++)
            memcpy(UV + r * w, sc + r * pc, 2 * cw <= pc ? 2 * cw : pc);
    } else if (pf == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
               pf == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange) {
        // 16-bit little-endian samples with the value in the HIGH bits.
        *full = pf == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
        const uint8_t *sy = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        const uint8_t *sc = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        size_t py = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
        size_t pc = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
        if (!sy || !sc) ok = 0;
        for (size_t r = 0; ok && r < h; r++)
            for (size_t x = 0; x < w; x++) Y[r * w + x] = sy[r * py + 2 * x + 1];
        for (size_t r = 0; ok && r < ch; r++)
            for (size_t x = 0; x < 2 * cw; x++) UV[r * w + x] = sc[r * pc + 2 * x + 1];
    } else if (pf == kCVPixelFormatType_32BGRA) {
        // BT.709 limited range, 2x2 box for chroma. Only reached with
        // KL_VTDEC_BGRA or on the simulator.
        *full = 0;
        const uint8_t *sp = CVPixelBufferGetBaseAddress(pb);
        size_t pr = CVPixelBufferGetBytesPerRow(pb);
        if (!sp) ok = 0;
        for (size_t r = 0; ok && r < h; r++) {
            const uint8_t *row = sp + r * pr;
            for (size_t x = 0; x < w; x++) {
                int B = row[4 * x], G = row[4 * x + 1], R = row[4 * x + 2];
                Y[r * w + x] = (uint8_t)(16 + ((47 * R + 157 * G + 16 * B + 128) >> 8));
            }
        }
        for (size_t r = 0; ok && r < ch; r++) {
            const uint8_t *row = sp + (2 * r) * pr;
            for (size_t x = 0; x < cw; x++) {
                size_t sx = 2 * x < w ? 2 * x : w - 1;
                int B = row[4 * sx], G = row[4 * sx + 1], R = row[4 * sx + 2];
                UV[r * w + 2 * x]     = (uint8_t)(128 + ((-26 * R - 87 * G + 112 * B + 128) >> 8));
                UV[r * w + 2 * x + 1] = (uint8_t)(128 + ((112 * R - 102 * G - 10 * B + 128) >> 8));
            }
        }
    } else {
        ok = 0;
        static int said;
        if (!said++)
            fprintf(stderr, "  [media] buffer mode: decoder produced pixel format "
                            "'%c%c%c%c', which has no NV12 conversion here — the "
                            "guest gets no picture\n",
                    (char)(pf >> 24), (char)(pf >> 16), (char)(pf >> 8), (char)pf);
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    // A frame we could not read still has to BE a frame. The guest adds its
    // plane offsets to whatever getOutputBuffer returns without testing it —
    // a NULL here was a memmove from address sliceHeight * stride (0x3f4800 for
    // this clip), i.e. a crash. Black is the honest stand-in.
    if (!ok) {
        memset(Y, 16, w * h);
        memset(UV, 128, w * ch);
    }
    return w * h + w * ch;
}

// A clip whose frame rate lies between half the display's and the display's
// own — 60 frames a second on a 100 Hz panel — cannot be shown evenly: each
// frame stands for one refresh or for two, 20-20-10 ms, and a slowly moving
// figure visibly judders. A Quest switches its panel to the 60 Hz the game asks
// for (SetSystemDisplayFrequency(60) in the log); visionOS's mixed space has
// only 100 Hz (or 50). So such a clip is put on a grid of two refreshes: every
// frame gets the presentation time of its nearest grid slot, and a frame whose
// slot is already taken is left out — one in six at 60 on 100. Each frame then
// stands exactly 20 ms, the picture never strays more than half a slot from the
// sound, and the motion steps once every 0.1 s instead of stuttering.
// KL_VIDEO_EVEN=0|1; on by default for LES MILLS BODYCOMBAT, whose owner chose
// this over a 120 Hz default (on device, 2026-10-15). At 120 Hz nothing changes.
// Returns 1 if the frame is to be dropped; otherwise *pts may be moved.
static int klm_even_cadence(klm_codec *c, int64_t *pts) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_VIDEO_EVEN", t && !strcmp(t, "x4015163475201433"));
    }
    if (!on || !c->surface) return 0;
    int64_t src = *pts, d = src - c->ec_last_src_pts;
    if (c->ec_last_src_pts && d > 0 && d < 100000)
        c->ec_period_us = c->ec_period_us ? (c->ec_period_us * 7 + d) / 8 : d;
    c->ec_last_src_pts = src;
    float hz = kl_ovrp_display_frequency();
    if (!c->ec_period_us || !(hz >= 30.0f)) return 0;
    double tick = 1e6 / hz, fps = 1e6 / (double)c->ec_period_us;
    // Strictly between half the panel rate and the panel rate, with a margin, so
    // a 30-frame clip on 60, or 60 on 120, is never touched.
    if (!(fps > hz * 0.55 && fps < hz * 0.97)) return 0;
    int64_t grid = (int64_t)(2.0 * tick + 0.5);
    int64_t slot = (src + grid / 2) / grid;
    if (c->ec_have_slot && slot <= c->ec_last_slot) {
        c->ec_dropped++;
        return 1;
    }
    if (!c->ec_have_slot)
        fprintf(stderr, "  [media] even cadence: a %.1f fps clip on a %.1f Hz display is "
                        "shown on a %.1f ms grid, one frame per two refreshes "
                        "(KL_VIDEO_EVEN)\n", fps, (double)hz, grid / 1000.0);
    c->ec_have_slot = 1;
    c->ec_last_slot = slot;
    *pts = slot * grid;
    return 0;
}

static ssize_t klm_AMediaCodec_dequeueOutputBuffer(void *codec,
                                                   AMediaCodecBufferInfo *info,
                                                   int64_t timeoutUs) {
    klm_codec *c = codec;
    if (!c || c->magic != KLM_CODEC_MAGIC) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (c->pcm) return klm_pcm_dequeue_output(c, info, timeoutUs);
    klm_codec_beat(c, "dequeue");
    int64_t waited = 0;
    for (;;) {
        pthread_mutex_lock(&c->lock);
        int stopped=c->playback_backpressure && !c->started;
        pthread_mutex_unlock(&c->lock);
        if (stopped) return AMEDIA_ERROR_INVALID_OPERATION;
        int64_t pts = 0;
        CVPixelBufferRef pb = kl_vtdec_pull(c->dec, &pts);
        if (pb && klm_even_cadence(c, &pts)) {
            CVPixelBufferRelease(pb);
            continue;
        }
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
                    // Buffer mode: flatten now, under the lock, so the slot's
                    // bytes are in place before the guest can ask for them.
                    size_t lin = 0;
                    if (!c->surface)
                        lin = klm_linearize(pb, &c->out[i].lin, &c->out[i].lin_cap,
                                            &c->lin_full_range);
                    c->out[i].lin_size = lin;
                    pthread_mutex_unlock(&c->lock);
                    if (!c->surface)
                        KLM_FIRST(said_lin, "buffer mode: first frame flattened to "
                                  "NV12, %dx%d, %zu bytes (row pitch = width)\n",
                                  c->out_w, c->out_h, lin);
                    KLM_FIRST(said_out, "first decoded frame dequeued (slot %d, "
                                        "pts %lld us)\n", i, (long long)pts);
                    if (info) {
                        info->offset = 0;
                        // Zero, and deliberately: this codec renders to a
                        // surface, so there is no CPU-visible output buffer and
                        // Android reports 0 too. A guest that memcpy'd `size`
                        // bytes out of it would be reading a buffer that does
                        // not exist on Android either.
                        info->size = (int32_t)lin;      // non-zero only in buffer mode
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
    if (c->pcm) {
        pthread_mutex_lock(&c->lock);
        // A buffer a flush already took back is released fine on Android too.
        if (c->pq[idx].state == 2) c->pq[idx].state = 0;
        pthread_mutex_unlock(&c->lock);
        return AMEDIA_OK;
    }
    klm_codec_beat(c, "release");
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
    // is how a silent reporting path reads.
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
    M("AMEDIAFORMAT_KEY_ROTATION",       &g_key_rotation),
    M("AMEDIAFORMAT_KEY_ENCODER_DELAY",  &g_key_encoder_delay),
    M("AMEDIAFORMAT_KEY_SLICE_HEIGHT",   &g_key_slice_height),

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
    M("AMediaExtractor_setDataSourceCustom", klm_AMediaExtractor_setDataSourceCustom),
    M("AMediaDataSource_new",                klm_AMediaDataSource_new),
    M("AMediaDataSource_delete",             klm_AMediaDataSource_delete),
    M("AMediaDataSource_setUserdata",        klm_AMediaDataSource_setUserdata),
    M("AMediaDataSource_setReadAt",          klm_AMediaDataSource_setReadAt),
    M("AMediaDataSource_setGetSize",         klm_AMediaDataSource_setGetSize),
    M("AMediaDataSource_setClose",           klm_AMediaDataSource_setClose),
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
    // where it CALLS rather than where it looked up.
    return fn ? fn : kl_named_stub(name, (void *)kl_unresolved_named);
}
