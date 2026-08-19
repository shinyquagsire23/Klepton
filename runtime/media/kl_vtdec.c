// See kl_vtdec.h for what this is and why the output is BGRA.
//
// The whole file is three conversions and a queue:
//
//   Annex-B -> parameter sets      -> CMVideoFormatDescription
//   Annex-B -> length-prefixed NALs -> CMBlockBuffer -> CMSampleBuffer
//   VideoToolbox's async callback   -> a small ring the guest pulls from
//
// Nothing here knows what AMediaCodec is.
#include "kl_vtdec.h"

#include "kl_env.h"

#include <TargetConditionals.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <VideoToolbox/VideoToolbox.h>

// ---------------------------------------------------------------------------
// Codec identity
//
// The two codecs differ in exactly three places — where the NAL type lives in
// the header byte, which type numbers are parameter sets, and which
// CMVideoFormatDescription constructor takes them — so they share everything
// else rather than being two decoders.
//
// HEVC is what this guest asks for (its QSVLCodecNDK::Init hardcodes
// "video/hevc"; measured). H.264 is here because the same three lines serve it,
// the APK carries libh264bitstream.so beside the HEVC one, and the alternative
// is aborting by name on a codec we can already decode.
enum { KLVT_HEVC, KLVT_AVC };

// HEVC: nal_unit_type is bits 6..1 of the first header byte. AVC: bits 4..0.
static int nal_type(int codec, uint8_t b0) {
    return codec == KLVT_HEVC ? (b0 >> 1) & 0x3f : b0 & 0x1f;
}

// Parameter-set slots, in the order the format-description constructors want
// them: VPS, SPS, PPS for HEVC; SPS, PPS for AVC (which has no VPS, so slot 0
// goes unused and n_sets is 2).
enum { KLVT_VPS, KLVT_SPS, KLVT_PPS, KLVT_NSETS };

static int param_slot(int codec, int type) {
    if (codec == KLVT_HEVC) {
        if (type == 32) return KLVT_VPS;
        if (type == 33) return KLVT_SPS;
        if (type == 34) return KLVT_PPS;
    } else {
        if (type == 7) return KLVT_SPS;
        if (type == 8) return KLVT_PPS;
    }
    return -1;
}

// ---------------------------------------------------------------------------

#define KLVT_RING 16            // decoded frames held before the oldest is dropped
#define KLVT_NAL_LEN_BYTES 4    // what we length-prefix with, and what we tell
                                // the format description we used. Must agree.

typedef struct { CVPixelBufferRef pb; int64_t pts_us; } klvt_frame;

struct kl_vtdec {
    int codec;
    int bgra;                   // sessions request 32BGRA (KL_VTDEC_BGRA=1, or
                                // the Simulator); 0 = decoder's native format

    // The most recent copy of each parameter set. Kept rather than pointed at:
    // they arrive inside a guest buffer that is reused the moment we return,
    // and a format description built from a dangling pointer fails much later
    // and somewhere else.
    uint8_t *set[KLVT_NSETS];
    size_t   set_len[KLVT_NSETS];
    int      sets_changed;      // a set arrived that differs from the one held

    CMVideoFormatDescriptionRef fmt;
    VTDecompressionSessionRef   sess;

    pthread_mutex_t lock;       // guards the ring and the counters only; the
                                // session is touched from the submitting thread
    klvt_frame ring[KLVT_RING];
    int ring_head, ring_count;

    unsigned n_submitted, n_decoded, n_dropped, n_failed;
    // Parameter-set bookkeeping. Separated because they answer different
    // questions: how often the host re-describes the stream (n_set_change),
    // how often that description is genuinely new (n_fmt_built), and how often
    // it was different enough to cost the session its reference frames
    // (n_sess_created). A run where the last of those tracks the first is a run
    // whose picture is being rebuilt from scratch several times a second.
    unsigned n_set_change[KLVT_NSETS];
    unsigned n_fmt_built, n_sess_created, n_fmt_swapped;
    int      logged_changes;
    int width, height;

    FILE    *dump;              // KL_VTDEC_DUMP — see klvt_open_dump
    FILE    *dump_idx;
    unsigned dump_n;
};

static kl_vtdec *g_last;        // for kl_vtdec_report; the guest makes one

// ---------------------------------------------------------------------------
// Annex-B iteration
//
// Walks NAL units in `buf`, tolerating both 3- and 4-byte start codes and a
// buffer that does not begin with one (in which case the whole thing is one
// NAL, which is what a caller that already stripped framing hands us).
//
// Returns the payload of the next NAL — start code excluded — and advances
// *pos. Iteration is over 3-byte start codes only, and the 4-byte form is
// handled by trimming trailing zeros off each payload instead of by matching
// two patterns: the extra leading zero of `00 00 00 01` is a trailing zero of
// whatever came before it, which is also exactly what the spec's
// trailing_zero_8bits are. One rule covers both.
static int find_sc(const uint8_t *b, size_t len, size_t from, size_t *at) {
    for (size_t j = from; j + 2 < len; j++)
        if (b[j] == 0 && b[j + 1] == 0 && b[j + 2] == 1) { *at = j; return 1; }
    return 0;
}

static const uint8_t *next_nal(const uint8_t *buf, size_t len, size_t *pos,
                               size_t *nal_len) {
    for (;;) {
        size_t sc;
        if (!find_sc(buf, len, *pos, &sc)) {
            // Nothing further. A buffer with no start code AT ALL is a bare
            // NAL, which is what a caller that already stripped its framing
            // hands us; anything else is the end of iteration.
            if (*pos == 0 && len) { *pos = len; *nal_len = len; return buf; }
            return NULL;
        }
        size_t start = sc + 3, end;
        if (!find_sc(buf, len, start, &end)) end = len;
        *pos = end;                       // resume AT the next start code
        size_t e = end;
        while (e > start && buf[e - 1] == 0) e--;
        if (e > start) { *nal_len = e - start; return buf + start; }
        if (end >= len) return NULL;      // an empty NAL at the very end
    }
}

// ---------------------------------------------------------------------------
// Parameter sets -> format description -> session

static void klvt_teardown_session(kl_vtdec *d) {
    if (d->sess) {
        VTDecompressionSessionWaitForAsynchronousFrames(d->sess);
        VTDecompressionSessionInvalidate(d->sess);
        CFRelease(d->sess);
        d->sess = NULL;
    }
    if (d->fmt) { CFRelease(d->fmt); d->fmt = NULL; }
}

static void klvt_output(void *decompressionOutputRefCon, void *sourceFrameRefCon,
                        OSStatus status, VTDecodeInfoFlags flags,
                        CVImageBufferRef image, CMTime pts, CMTime duration) {
    (void)sourceFrameRefCon; (void)flags; (void)duration;
    kl_vtdec *d = (kl_vtdec *)decompressionOutputRefCon;

    if (status != noErr || !image) {
        pthread_mutex_lock(&d->lock);
        d->n_failed++;
        pthread_mutex_unlock(&d->lock);
        if (status != noErr)
            fprintf(stderr, "  [vtdec] decode failed: OSStatus %d\n", (int)status);
        return;
    }

    pthread_mutex_lock(&d->lock);
    d->n_decoded++;
    d->width  = (int)CVPixelBufferGetWidth(image);
    d->height = (int)CVPixelBufferGetHeight(image);

    // A full ring means the guest is not pulling as fast as we decode. For live
    // video the right answer is to drop the OLDEST — a late frame has no value
    // — and to say so, because silent dropping reads as a decoder that is
    // merely slow.
    if (d->ring_count == KLVT_RING) {
        CVPixelBufferRelease(d->ring[d->ring_head].pb);
        d->ring_head = (d->ring_head + 1) % KLVT_RING;
        d->ring_count--;
        d->n_dropped++;
    }
    int slot = (d->ring_head + d->ring_count) % KLVT_RING;
    d->ring[slot].pb = (CVPixelBufferRef)CFRetain(image);
    d->ring[slot].pts_us = (int64_t)(CMTimeGetSeconds(pts) * 1e6 + 0.5);
    d->ring_count++;
    pthread_mutex_unlock(&d->lock);
}

// Build the format description from whatever sets are held, and put a session
// on top of it only if the one we have cannot carry on.
//
// The distinction is the whole point of this function. A parameter set whose
// BYTES changed is not necessarily a stream whose DECODER changed: this host
// re-describes the stream constantly (measured: 498 changes across 2453 access
// units, i.e. every fifth frame), and tearing the VTDecompressionSession down
// for each one throws away every reference frame it holds — so the picture is
// rebuilt from an intra refresh several times a second and looks like a bad
// connection. VideoToolbox answers the actual question directly:
// VTDecompressionSessionCanAcceptFormatDescription says whether the session can
// keep decoding against the new description. When it can, the description is
// simply swapped — subsequent sample buffers carry the new one — and the
// reference frames survive. Only a genuine change (a resolution change, which
// this guest does do) costs a session.
static int klvt_rebuild(kl_vtdec *d) {
    int n_sets = d->codec == KLVT_HEVC ? 3 : 2;
    int first  = d->codec == KLVT_HEVC ? KLVT_VPS : KLVT_SPS;
    for (int i = 0; i < n_sets; i++)
        if (!d->set[first + i]) return 0;      // not complete yet; not an error

    const uint8_t *ptr[KLVT_NSETS];
    size_t         siz[KLVT_NSETS];
    for (int i = 0; i < n_sets; i++) {
        ptr[i] = d->set[first + i];
        siz[i] = d->set_len[first + i];
    }

    OSStatus st;
    CMVideoFormatDescriptionRef fmt = NULL;
    if (d->codec == KLVT_HEVC)
        st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                 kCFAllocatorDefault, (size_t)n_sets, ptr, siz,
                 KLVT_NAL_LEN_BYTES, NULL, &fmt);
    else
        st = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                 kCFAllocatorDefault, (size_t)n_sets, ptr, siz,
                 KLVT_NAL_LEN_BYTES, &fmt);
    if (st != noErr) {
        fprintf(stderr, "  [vtdec] format description failed: OSStatus %d\n", (int)st);
        return -1;
    }
    d->n_fmt_built++;

    // Identical descriptions happen whenever the changed bytes were ones the
    // format description does not carry. Nothing to do at all, not even a swap.
    if (d->fmt && d->sess && CMFormatDescriptionEqual(d->fmt, fmt)) {
        CFRelease(fmt);
        return 0;
    }

    // Safe to swap without a lock: d->fmt is read only where sample buffers are
    // built, which is this same submitting thread. The async output callback
    // never touches it.
    if (d->sess && VTDecompressionSessionCanAcceptFormatDescription(d->sess, fmt)) {
        if (d->fmt) CFRelease(d->fmt);
        d->fmt = fmt;
        d->n_fmt_swapped++;
        if (d->n_fmt_swapped == 1)
            fprintf(stderr, "  [vtdec] parameter sets changed; the session accepts "
                            "the new description — reference frames kept\n");
        return 0;
    }

    klvt_teardown_session(d);
    d->fmt = fmt;

    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(d->fmt);

    // What we want back. By default: no pixel format at all. Requesting one is
    // itself a cost twice over — the conversion out of the decoder's native
    // biplanar 4:2:0 (BGRA is 2.67x the bytes at Steam Link's 1536x6144), and
    // the copy VideoToolbox performs on visionOS 2 merely because a format was
    // named (~2 ms/frame at high resolution, measured by the VisionOSALVRClient
    // author). The native frame is served to the guest by kl_glfb.c through
    // Apple's private single-plane YCbCr MTLPixelFormats, where the SAMPLER
    // does the YUV->RGB — the samplerExternalOES promise kept in hardware.
    //
    // KL_VTDEC_BGRA=1 restores the BGRA request (the A/B, and the escape hatch
    // for a native format kl_glfb cannot wrap). The Simulator always takes it:
    // the private formats do not exist there. The Metal key is what makes the
    // result usable as an MTLTexture without a copy either way; the IOSurface
    // key rides with BGRA because the pbuffer path needs IOSurface backing
    // explicitly.
#if TARGET_OS_SIMULATOR
    d->bgra = 1;
#else
    d->bgra = kl_env_on("KL_VTDEC_BGRA", 0);
#endif
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
    if (d->bgra) {
        int32_t pf = kCVPixelFormatType_32BGRA;
        CFNumberRef pfn = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pf);
        CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, pfn);
        CFRelease(pfn);
        CFDictionaryRef empty = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, empty);
        CFRelease(empty);
    } else {
        // Keeps the decoder from stalling on buffer churn while frames sit in
        // our ring; the count is the reference client's.
        int32_t mincount = 3;
        CFNumberRef mcn = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &mincount);
        CFDictionarySetValue(attrs, kCVPixelBufferPoolMinimumBufferCountKey, mcn);
        CFRelease(mcn);
    }

    VTDecompressionOutputCallbackRecord cb = { klvt_output, d };
    st = VTDecompressionSessionCreate(kCFAllocatorDefault, d->fmt, NULL, attrs,
                                      &cb, &d->sess);
    CFRelease(attrs);
    if (st != noErr) {
        fprintf(stderr, "  [vtdec] session create failed: OSStatus %d\n", (int)st);
        CFRelease(d->fmt); d->fmt = NULL; d->sess = NULL;
        return -1;
    }

    d->n_sess_created++;
    fprintf(stderr, "  [vtdec] %s decoder ready: %dx%d -> %s%s\n",
            d->codec == KLVT_HEVC ? "HEVC" : "H.264", dim.width, dim.height,
            d->bgra ? "BGRA" : "native format (none requested)",
            d->n_sess_created > 1 ? " (session rebuilt — reference frames lost)" : "");
    d->width = dim.width; d->height = dim.height;
    return 0;
}

// ---------------------------------------------------------------------------

// KL_VTDEC_DUMP=<path> — write the elementary stream exactly as the guest
// queued it, plus a sidecar index of one line per access unit.
//
// This exists because every question left about the picture (what bitrate the
// host chose, how often it sends an IRAP, why the parameter sets change) is a
// question about the BITSTREAM, and the only way to see the bitstream today is
// a live streaming run — which costs a fresh Steam pairing, cannot be repeated
// identically, and takes the answer with it when it ends. A dump turns all of
// them into offline questions, and gives `make hevc` a corpus recorded from the
// real host rather than from ffmpeg.
//
// The stream file is plain Annex-B, so ffprobe reads it directly. The index is
// `<n> <pts_us> <bytes>` per access unit, which is what makes instantaneous
// bitrate measurable without parsing anything.
static void klvt_open_dump(kl_vtdec *d) {
    const char *path = getenv("KL_VTDEC_DUMP");
    if (!path || !*path) return;
    d->dump = fopen(path, "wb");
    if (!d->dump) {
        fprintf(stderr, "  [vtdec] KL_VTDEC_DUMP: cannot write %s\n", path);
        return;
    }
    char idx[1024];
    snprintf(idx, sizeof idx, "%s.idx", path);
    d->dump_idx = fopen(idx, "w");
    if (d->dump_idx)
        fprintf(d->dump_idx, "# access_unit pts_us bytes\n");
    fprintf(stderr, "  [vtdec] dumping the elementary stream to %s\n", path);
}

static void klvt_dump_au(kl_vtdec *d, const uint8_t *annexb, size_t len,
                         int64_t pts_us) {
    if (!d->dump) return;
    // A buffer that already begins with a start code is written verbatim; one
    // that does not is a bare NAL (see next_nal) and gets the four-byte form in
    // front of it, so the file stays a legal elementary stream either way.
    int has_sc = len >= 3 && annexb[0] == 0 && annexb[1] == 0 &&
                 (annexb[2] == 1 || (len >= 4 && annexb[2] == 0 && annexb[3] == 1));
    if (!has_sc) fwrite("\0\0\0\1", 1, 4, d->dump);
    fwrite(annexb, 1, len, d->dump);
    if (d->dump_idx)
        fprintf(d->dump_idx, "%u %lld %zu\n", d->dump_n, (long long)pts_us, len);
    d->dump_n++;
}

kl_vtdec *kl_vtdec_create(const char *mime) {
    int codec;
    if (mime && (!strcmp(mime, "video/hevc") || !strcmp(mime, "video/x-hevc")))
        codec = KLVT_HEVC;
    else if (mime && !strcmp(mime, "video/avc"))
        codec = KLVT_AVC;
    else
        return NULL;            // the caller reports it; see the header

    kl_vtdec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->codec = codec;
    pthread_mutex_init(&d->lock, NULL);
    klvt_open_dump(d);
    g_last = d;
    return d;
}

void kl_vtdec_destroy(kl_vtdec *d) {
    if (!d) return;
    klvt_teardown_session(d);
    kl_vtdec_flush(d);
    if (d->dump)     fclose(d->dump);
    if (d->dump_idx) fclose(d->dump_idx);
    for (int i = 0; i < KLVT_NSETS; i++) free(d->set[i]);
    pthread_mutex_destroy(&d->lock);
    if (g_last == d) g_last = NULL;
    free(d);
}

// The leading ue(v) of a parameter set's RBSP, which for a PPS of either codec
// is its id. Only the first few bits are read, and emulation prevention cannot
// have touched them (it needs two preceding zero bytes), so no unescaping is
// needed. -1 when the field does not fit or is not one we can name cheaply.
static int klvt_leading_ue(const uint8_t *p, size_t n, size_t hdr) {
    if (n <= hdr) return -1;
    unsigned zeros = 0, bit = 0;
    size_t byte = hdr;
    while (byte < n && zeros < 16) {
        int b = (p[byte] >> (7 - bit)) & 1;
        if (++bit == 8) { bit = 0; byte++; }
        if (b) break;
        zeros++;
    }
    if (zeros >= 16) return -1;
    unsigned v = 1;
    for (unsigned i = 0; i < zeros; i++) {
        if (byte >= n) return -1;
        v = (v << 1) | ((p[byte] >> (7 - bit)) & 1);
        if (++bit == 8) { bit = 0; byte++; }
    }
    return (int)(v - 1);
}

static const char *klvt_slot_name(int slot) {
    return slot == KLVT_VPS ? "VPS" : slot == KLVT_SPS ? "SPS" : "PPS";
}

// Hold a parameter set, and say whether it is different from the one held.
// Comparing rather than always rebuilding matters: this guest re-sends its
// parameter sets in front of every IDR, which is once a second or so, and
// tearing the session down that often would drop every frame in flight.
//
// When it IS different, say how — which set, whether it grew, and where the
// first differing byte is. That last one is the diagnosis: a difference at
// byte 0..2 is a set with a different id (the stream carries more than one and
// we are holding a single slot per kind, so they alternate), and a difference
// deep in the tail is one field of an otherwise identical set moving. The two
// have completely different fixes, and the counters alone cannot tell them
// apart. Rate-limited, because the whole problem is that this happens often.
#define KLVT_CHANGE_LOG 8
static int klvt_hold(kl_vtdec *d, int slot, const uint8_t *p, size_t n) {
    if (d->set[slot] && d->set_len[slot] == n && !memcmp(d->set[slot], p, n))
        return 0;

    d->n_set_change[slot]++;
    if (d->set[slot] && d->logged_changes < KLVT_CHANGE_LOG) {
        d->logged_changes++;
        size_t common = d->set_len[slot] < n ? d->set_len[slot] : n, at = 0;
        while (at < common && d->set[slot][at] == p[at]) at++;
        size_t hdr = d->codec == KLVT_HEVC ? 2 : 1;
        int was = slot == KLVT_PPS
                    ? klvt_leading_ue(d->set[slot], d->set_len[slot], hdr) : -1;
        int now = slot == KLVT_PPS ? klvt_leading_ue(p, n, hdr) : -1;
        if (was >= 0 || now >= 0)
            fprintf(stderr, "  [vtdec] %s changed: %zu -> %zu bytes, first "
                            "difference at byte %zu, pps_id %d -> %d\n",
                    klvt_slot_name(slot), d->set_len[slot], n, at, was, now);
        else
            fprintf(stderr, "  [vtdec] %s changed: %zu -> %zu bytes, first "
                            "difference at byte %zu\n",
                    klvt_slot_name(slot), d->set_len[slot], n, at);
    }

    uint8_t *copy = malloc(n ? n : 1);
    if (!copy) return 0;
    memcpy(copy, p, n);
    free(d->set[slot]);
    d->set[slot] = copy;
    d->set_len[slot] = n;
    return 1;
}

int kl_vtdec_submit(kl_vtdec *d, const uint8_t *annexb, size_t len,
                    int64_t pts_us) {
    if (!d || !annexb || !len) return -1;
    klvt_dump_au(d, annexb, len, pts_us);

    // Pass 1: absorb parameter sets, and measure how much length-prefixed
    // payload the rest will need. Two passes rather than one so the block
    // buffer is allocated exactly once.
    size_t pos = 0, payload = 0, n_nals = 0;
    const uint8_t *nal;
    size_t nal_len;
    d->sets_changed = 0;
    while ((nal = next_nal(annexb, len, &pos, &nal_len)) != NULL) {
        int slot = param_slot(d->codec, nal_type(d->codec, nal[0]));
        if (slot >= 0) {
            d->sets_changed |= klvt_hold(d, slot, nal, nal_len);
            continue;
        }
        payload += KLVT_NAL_LEN_BYTES + nal_len;
        n_nals++;
    }

    if (d->sets_changed || (!d->sess && d->set[KLVT_SPS])) {
        if (klvt_rebuild(d) < 0) return -1;
    }

    // An access unit that was nothing but parameter sets is a codec-config
    // buffer and is complete. Saying so is not the same as failing.
    if (!n_nals) return 0;
    if (!d->sess) return 0;     // sets still incomplete — the stream has not
                                // reached its first IRAP. Normal at join time.

    uint8_t *buf = malloc(payload);
    if (!buf) return -1;
    size_t off = 0;
    pos = 0;
    while ((nal = next_nal(annexb, len, &pos, &nal_len)) != NULL) {
        if (param_slot(d->codec, nal_type(d->codec, nal[0])) >= 0) continue;
        buf[off + 0] = (uint8_t)(nal_len >> 24);
        buf[off + 1] = (uint8_t)(nal_len >> 16);
        buf[off + 2] = (uint8_t)(nal_len >> 8);
        buf[off + 3] = (uint8_t)(nal_len);
        memcpy(buf + off + KLVT_NAL_LEN_BYTES, nal, nal_len);
        off += KLVT_NAL_LEN_BYTES + nal_len;
    }

    // kCFAllocatorMalloc as the block allocator hands `buf` to CoreMedia, which
    // frees it when the block buffer dies. Do not free it here.
    CMBlockBufferRef bb = NULL;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault, buf, off, kCFAllocatorMalloc, NULL, 0, off, 0, &bb);
    if (st != noErr) { free(buf); return -1; }

    CMSampleTimingInfo timing = {
        .duration = kCMTimeInvalid,
        .presentationTimeStamp = CMTimeMake(pts_us, 1000000),
        .decodeTimeStamp = kCMTimeInvalid,
    };
    CMSampleBufferRef sb = NULL;
    st = CMSampleBufferCreateReady(kCFAllocatorDefault, bb, d->fmt, 1, 1,
                                   &timing, 1, &off, &sb);
    CFRelease(bb);
    if (st != noErr) return -1;

    VTDecodeInfoFlags info = 0;
    st = VTDecompressionSessionDecodeFrame(
        d->sess, sb, kVTDecodeFrame_EnableAsynchronousDecompression, NULL, &info);
    CFRelease(sb);

    pthread_mutex_lock(&d->lock);
    d->n_submitted++;
    pthread_mutex_unlock(&d->lock);

    if (st != noErr) {
        fprintf(stderr, "  [vtdec] decode submit failed: OSStatus %d\n", (int)st);
        return -1;
    }
    return 0;
}

CVPixelBufferRef kl_vtdec_pull(kl_vtdec *d, int64_t *pts_us_out) {
    if (!d) return NULL;
    pthread_mutex_lock(&d->lock);
    CVPixelBufferRef pb = NULL;
    if (d->ring_count) {
        pb = d->ring[d->ring_head].pb;
        if (pts_us_out) *pts_us_out = d->ring[d->ring_head].pts_us;
        d->ring[d->ring_head].pb = NULL;
        d->ring_head = (d->ring_head + 1) % KLVT_RING;
        d->ring_count--;
    }
    pthread_mutex_unlock(&d->lock);
    return pb;                  // the caller now owns the retain taken in klvt_output
}

void kl_vtdec_flush(kl_vtdec *d) {
    if (!d) return;
    if (d->sess) VTDecompressionSessionFinishDelayedFrames(d->sess);
    pthread_mutex_lock(&d->lock);
    while (d->ring_count) {
        CVPixelBufferRelease(d->ring[d->ring_head].pb);
        d->ring[d->ring_head].pb = NULL;
        d->ring_head = (d->ring_head + 1) % KLVT_RING;
        d->ring_count--;
    }
    pthread_mutex_unlock(&d->lock);
}

void kl_vtdec_stats(const kl_vtdec *d, unsigned *submitted, unsigned *decoded,
                    unsigned *dropped, int *width, int *height) {
    if (!d) return;
    // Reading the counters without the lock would be a data race for the sake
    // of a diagnostic; taking it is free at the rate anyone asks.
    pthread_mutex_lock((pthread_mutex_t *)&d->lock);
    if (submitted) *submitted = d->n_submitted;
    if (decoded)   *decoded   = d->n_decoded;
    if (dropped)   *dropped   = d->n_dropped;
    if (width)     *width     = d->width;
    if (height)    *height    = d->height;
    pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
}

void kl_vtdec_param_stats(const kl_vtdec *d, unsigned *built, unsigned *swapped,
                          unsigned *sessions) {
    if (!d) return;
    pthread_mutex_lock((pthread_mutex_t *)&d->lock);
    if (built)    *built    = d->n_fmt_built;
    if (swapped)  *swapped  = d->n_fmt_swapped;
    if (sessions) *sessions = d->n_sess_created;
    pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
}

void kl_vtdec_report(FILE *f) {
    kl_vtdec *d = g_last;
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    fprintf(f, "\n=== video decoder (VideoToolbox) ===\n");
    fprintf(f, "  %s %dx%d: %u submitted, %u decoded, %u dropped, %u failed,"
               " %d held\n",
            d->codec == KLVT_HEVC ? "HEVC" : "H.264", d->width, d->height,
            d->n_submitted, d->n_decoded, d->n_dropped, d->n_failed,
            d->ring_count);
    // The parameter-set line, and what to read into it: sessions is the number
    // that costs a picture. One is the healthy answer for a stream that never
    // changes resolution, however often the host re-describes it.
    fprintf(f, "  parameter sets: %u VPS / %u SPS / %u PPS changes -> "
               "%u descriptions built, %u swapped into the live session, "
               "%u session%s created\n",
            d->n_set_change[KLVT_VPS], d->n_set_change[KLVT_SPS],
            d->n_set_change[KLVT_PPS], d->n_fmt_built, d->n_fmt_swapped,
            d->n_sess_created, d->n_sess_created == 1 ? "" : "s");
    if (d->dump)
        fprintf(f, "  %u access units dumped to KL_VTDEC_DUMP\n", d->dump_n);
    pthread_mutex_unlock(&d->lock);
}
