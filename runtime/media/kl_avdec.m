// kl_avdec — the host video decoder behind UE4's Java media player. See
// kl_avdec.h for why this is AVFoundation and not kl_vtdec.
//
// Shape: one decoder thread per player, pulling samples out of an
// AVAssetReader and publishing the newest one as tightly-packed BGRA. The
// thread paces itself against the sample's own presentation timestamp, because
// the guest polls for "the current frame" on ITS clock and never asks for a
// specific time — so if this handed out frames as fast as they decode, a
// 30-second cutscene would be over in under a second and the game would carry
// on into a scene the player never saw.
//
// The pixels are COPIED out of the CVPixelBuffer rather than handed over
// in place, and that is not laziness. A CVPixelBuffer's rows are padded to the
// decoder's alignment (measured 2048 for a 1920-wide H.265 frame here), and
// UE4 reads the buffer as `width * height * 4` tightly packed — so handing the
// base address over directly gives a picture that skews further right on every
// row, which is a recognisable failure that looks like a decode bug.

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "kl_avdec.h"
#include "kl_env.h"

struct kl_avdec {
    char        path[1024];          // the file actually opened (see slice_out)
    int         width, height;
    long long   duration_ms;

    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_mutex_t fmu;        // held across a frame write and a reader's copy
    pthread_cond_t  cv;
    int         running;             // the thread should keep going
    int         playing;
    int         looping;
    int         complete;
    long long   position_ms;
    long long   seek_ms;             // >= 0: a seek is pending
    unsigned long long serial;

    unsigned char *frame;            // tightly packed BGRA, width*height*4
    size_t         frame_bytes;
};

// ---------------------------------------------------------------------------
// A byte range inside a container, as a file AVFoundation can open.
//
// UE4 addresses a movie as (container, offset, length) because on Android it
// lives inside the OBB, and AVURLAsset has no notion of a range. The four MP4s
// in RE4's OBB are `Stored`, i.e. a contiguous uncompressed span, so the slice
// is a straight copy — done once and cached, keyed on all three values so a
// different range of the same container cannot be served the previous one.
//
// Into TMPDIR rather than the guest's own files directory: this is a cache of
// something the OBB already holds, it is regenerable, and on visionOS TMPDIR is
// the container's tmp — which is the one place the system is allowed to reclaim
// behind us.
static int slice_out(const char *container, long long off, long long len,
                     char *out, size_t outsz) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    const char *base = strrchr(container, '/');
    base = base ? base + 1 : container;
    snprintf(out, outsz, "%s/klepton-media-%s-%lld-%lld.mp4", tmp, base, off, len);

    struct stat st;
    if (stat(out, &st) == 0 && st.st_size == (off_t)len) return 1;   // cached

    int in = open(container, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "  [avdec] cannot open the container %s: %s\n",
                container, strerror(errno));
        return 0;
    }
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        close(in);
        fprintf(stderr, "  [avdec] cannot write the media cache %s: %s\n",
                out, strerror(errno));
        return 0;
    }
    // 1 MiB at a time: these are tens of megabytes and this runs on the guest's
    // own thread inside setDataSource, which it is entitled to have block.
    enum { CHUNK = 1 << 20 };
    unsigned char *buf = malloc(CHUNK);
    long long left = len;
    int ok = buf != NULL;
    while (ok && left > 0) {
        size_t want = left < CHUNK ? (size_t)left : CHUNK;
        ssize_t got = pread(in, buf, want, (off_t)(off + (len - left)));
        // A short read is not an error from pread and IS a truncated movie, so
        // it has to be treated as one — the failure otherwise arrives as
        // AVFoundation refusing a file for reasons that name nothing.
        if (got <= 0) { ok = 0; break; }
        if (write(fd, buf, (size_t)got) != got) { ok = 0; break; }
        left -= got;
    }
    free(buf);
    close(fd);
    close(in);
    if (!ok) {
        unlink(out);
        fprintf(stderr, "  [avdec] could not extract %lld bytes at %lld from %s\n",
                len, off, container);
        return 0;
    }
    fprintf(stderr, "  [avdec] extracted %lld bytes at %lld of %s -> %s\n",
            len, off, base, out);
    return 1;
}

// ---------------------------------------------------------------------------

// The asset's first video track, waited for.
//
// `tracksWithMediaType:` is merely deprecated on macOS and **unavailable** on
// visionOS, so the synchronous form is not an option for a file in
// RUNTIME_SHIP — which is the whole reason this helper exists rather than the
// one-liner. Blocking is correct here: both callers are already on a thread
// that owns the wait (the guest's own thread inside setDataSource, and the
// decode thread before its first sample).
static AVAssetTrack *first_track_of(AVAsset *asset, AVMediaType type) {
    __block AVAssetTrack *track = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:type
                 completionHandler:^(NSArray<AVAssetTrack *> *tracks, NSError *err) {
        (void)err;
        // Retained across the semaphore: the array the completion handler is
        // given is released when it returns. This file is compiled WITH ARC
        // (every driver passes -fobjc-arc over the whole set), so the
        // __block strong reference is what does the retaining — writing it by
        // hand does not compile at all.
        track = tracks.firstObject;
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    return track;
}
static AVAssetTrack *first_video_track(AVAsset *asset) {
    return first_track_of(asset, AVMediaTypeVideo);
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Copy one decoded frame out, tightly packed. Called with the lock NOT held.
// It takes `fmu` around the write, so a reader inside kl_avdec_frame_lock never
// copies a frame that is half the old one and half the next.
static void publish(kl_avdec *d, CVPixelBufferRef pb, long long pts_ms) {
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
    size_t src_stride = CVPixelBufferGetBytesPerRow(pb);
    const unsigned char *src = CVPixelBufferGetBaseAddress(pb);
    size_t need = w * h * 4;
    if (src && need) {
        pthread_mutex_lock(&d->fmu);
        pthread_mutex_lock(&d->mu);
        if (d->frame_bytes != need) {
            free(d->frame);
            d->frame = malloc(need);
            d->frame_bytes = d->frame ? need : 0;
        }
        if (d->frame) {
            for (size_t y = 0; y < h; y++)
                memcpy(d->frame + y * w * 4, src + y * src_stride, w * 4);
            d->width = (int)w;
            d->height = (int)h;
            d->position_ms = pts_ms;
            d->serial++;
        }
        pthread_mutex_unlock(&d->mu);
        pthread_mutex_unlock(&d->fmu);
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
}

static AVAssetReader *make_reader(AVAsset *asset, AVAssetTrack *track,
                                  CMTime from, NSError **err) {
    AVAssetReader *r = [AVAssetReader assetReaderWithAsset:asset error:err];
    if (!r) return nil;
    // BGRA out of the decoder rather than the native 4:2:0: UE4 reads this
    // buffer as packed RGBA-family pixels and has no plane handling on this
    // path at all, so the conversion has to happen somewhere and VideoToolbox
    // does it on the way out for free.
    NSDictionary *settings = @{
        (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (id)kCVPixelBufferMetalCompatibilityKey : @YES,
    };
    AVAssetReaderTrackOutput *out =
        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track
                                                   outputSettings:settings];
    out.alwaysCopiesSampleData = NO;
    if (![r canAddOutput:out]) return nil;
    [r addOutput:out];
    if (CMTIME_IS_VALID(from) && CMTimeGetSeconds(from) > 0)
        r.timeRange = CMTimeRangeMake(from, kCMTimePositiveInfinity);
    if (![r startReading]) return nil;
    return r;
}

static void *decode_thread(void *arg) {
    kl_avdec *d = arg;
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:
                          [NSString stringWithUTF8String:d->path]];
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
        AVAssetTrack *track = first_video_track(asset);
        if (!track) return NULL;

        CMTime start = kCMTimeZero;
        AVAssetReader *reader = nil;
        AVAssetReaderTrackOutput *out = nil;
        double clock0 = 0;          // wall time the current run began
        double clock_pts0 = 0;      // the pts that instant corresponds to
        int have_clock = 0;

        for (;;) {
            pthread_mutex_lock(&d->mu);
            while (d->running && !d->playing && d->seek_ms < 0)
                pthread_cond_wait(&d->cv, &d->mu);
            int run = d->running;
            long long seek = d->seek_ms;
            d->seek_ms = -1;
            pthread_mutex_unlock(&d->mu);
            if (!run) break;

            if (seek >= 0) {
                start = CMTimeMake(seek, 1000);
                reader = nil; out = nil; have_clock = 0;
                pthread_mutex_lock(&d->mu);
                d->complete = 0;
                d->position_ms = seek;
                pthread_mutex_unlock(&d->mu);
            }

            if (!reader) {
                NSError *err = nil;
                reader = make_reader(asset, track, start, &err);
                if (!reader) {
                    fprintf(stderr, "  [avdec] the reader could not be started: %s\n",
                            err.localizedDescription.UTF8String ?: "?");
                    pthread_mutex_lock(&d->mu);
                    d->complete = 1;
                    pthread_mutex_unlock(&d->mu);
                    break;
                }
                out = (AVAssetReaderTrackOutput *)reader.outputs.firstObject;
            }

            CMSampleBufferRef sb = [out copyNextSampleBuffer];
            if (!sb) {
                // End of stream. Looping restarts from the top; otherwise the
                // guest is told, once and permanently — a completion flag that
                // cleared itself would be a movie that never ends.
                if (d->looping) {
                    start = kCMTimeZero; reader = nil; out = nil; have_clock = 0;
                    continue;
                }
                pthread_mutex_lock(&d->mu);
                d->complete = 1;
                d->playing = 0;
                pthread_mutex_unlock(&d->mu);
                continue;
            }
            @autoreleasepool {
                CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
                double pts_s = CMTIME_IS_VALID(pts) ? CMTimeGetSeconds(pts) : 0;
                if (!have_clock) {
                    have_clock = 1;
                    clock0 = now_seconds();
                    clock_pts0 = pts_s;
                }
                // Pace to the stream's own timeline. Ahead of it, wait; behind
                // it, publish immediately and let the next frame catch up —
                // dropping would be the alternative and there is nothing to
                // drop to, because the guest only ever sees the newest frame.
                double due = clock0 + (pts_s - clock_pts0);
                double slack = due - now_seconds();
                if (slack > 0) usleep((useconds_t)(slack * 1e6));

                CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sb);
                if (pb) publish(d, pb, (long long)(pts_s * 1000.0));
            }
            CFRelease(sb);

            // A pause has to stop the CLOCK as well as the loop, or the frames
            // decoded after it resumes are all late and play back at once.
            pthread_mutex_lock(&d->mu);
            int paused = d->running && !d->playing && d->seek_ms < 0;
            pthread_mutex_unlock(&d->mu);
            if (paused) have_clock = 0;
        }
    }
    return NULL;
}

kl_avdec *kl_avdec_open(const char *path, long long offset, long long size) {
    if (!path || !*path) return NULL;
    char real[1024];
    if (offset > 0 || size > 0) {
        if (!slice_out(path, offset, size, real, sizeof real)) return NULL;
    } else {
        snprintf(real, sizeof real, "%s", path);
    }

    __block int w = 0, h = 0;
    __block long long dur = 0;
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:real]];
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
        AVAssetTrack *t = first_video_track(asset);
        if (!t) {
            // Named, because "there is no video track" and "the file is not
            // there" are the same NULL to the caller and a completely
            // different thing to fix.
            fprintf(stderr, "  [avdec] %s carries no video track — not opened\n", real);
            return NULL;
        }
        CGSize sz = t.naturalSize;
        w = (int)sz.width; h = (int)sz.height;
        dur = (long long)(CMTimeGetSeconds(asset.duration) * 1000.0);
    }
    if (w <= 0 || h <= 0) return NULL;

    kl_avdec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    snprintf(d->path, sizeof d->path, "%s", real);
    d->width = w; d->height = h; d->duration_ms = dur;
    d->seek_ms = -1;
    d->running = 1;
    pthread_mutex_init(&d->mu, NULL);
    pthread_mutex_init(&d->fmu, NULL);
    pthread_cond_init(&d->cv, NULL);
    if (pthread_create(&d->thread, NULL, decode_thread, d) != 0) {
        free(d);
        return NULL;
    }
    fprintf(stderr, "  [avdec] %s: %dx%d, %lld ms\n", d->path, w, h, dur);
    return d;
}

void kl_avdec_close(kl_avdec *d) {
    if (!d) return;
    pthread_mutex_lock(&d->mu);
    d->running = 0;
    d->playing = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mu);
    pthread_join(d->thread, NULL);
    free(d->frame);
    pthread_mutex_destroy(&d->mu);
    pthread_mutex_destroy(&d->fmu);
    pthread_cond_destroy(&d->cv);
    free(d);
}

int kl_avdec_width(kl_avdec *d)  { return d ? d->width : 0; }
int kl_avdec_height(kl_avdec *d) { return d ? d->height : 0; }
long long kl_avdec_duration_ms(kl_avdec *d) { return d ? d->duration_ms : 0; }

void kl_avdec_play(kl_avdec *d, int on) {
    if (!d) return;
    pthread_mutex_lock(&d->mu);
    d->playing = on != 0;
    if (on) d->complete = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mu);
}

void kl_avdec_seek(kl_avdec *d, long long ms) {
    if (!d) return;
    pthread_mutex_lock(&d->mu);
    d->seek_ms = ms < 0 ? 0 : ms;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mu);
}

void kl_avdec_set_looping(kl_avdec *d, int on) { if (d) d->looping = on != 0; }

long long kl_avdec_position_ms(kl_avdec *d) {
    if (!d) return 0;
    pthread_mutex_lock(&d->mu);
    long long p = d->position_ms;
    pthread_mutex_unlock(&d->mu);
    return p;
}

int kl_avdec_playing(kl_avdec *d) {
    if (!d) return 0;
    pthread_mutex_lock(&d->mu);
    int p = d->playing;
    pthread_mutex_unlock(&d->mu);
    return p;
}

int kl_avdec_complete(kl_avdec *d) {
    if (!d) return 0;
    pthread_mutex_lock(&d->mu);
    int c = d->complete;
    pthread_mutex_unlock(&d->mu);
    return c;
}

void kl_avdec_frame_lock(kl_avdec *d)   { if (d) pthread_mutex_lock(&d->fmu); }
void kl_avdec_frame_unlock(kl_avdec *d) { if (d) pthread_mutex_unlock(&d->fmu); }

const void *kl_avdec_frame(kl_avdec *d, int *w, int *h, size_t *bytes,
                           unsigned long long *serial) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (bytes) *bytes = 0;
    if (serial) *serial = 0;
    if (!d) return NULL;
    pthread_mutex_lock(&d->mu);
    const void *p = d->frame;
    if (w) *w = d->width;
    if (h) *h = d->height;
    if (bytes) *bytes = d->frame_bytes;
    if (serial) *serial = d->serial;
    pthread_mutex_unlock(&d->mu);
    return p;
}

// ===========================================================================
// Demux-only path for Unity's NDK video (AMediaExtractor). See kl_avdec.h.
//
// AVAssetReader with nil output settings yields the track's COMPRESSED samples,
// but in AVCC framing (4-byte length prefixes, parameter sets held out of band
// in the format description). kl_vtdec — behind AMediaCodec — decodes Annex-B
// (start-code NALs, parameter sets in-band), so every sample is converted here:
// length prefixes become 00 00 00 01 start codes, and the SPS/PPS(/VPS) are
// prepended on every keyframe so the decoder can build its format from the
// stream exactly as it does for Steam Link.
// ===========================================================================

struct kl_avdemux {
    char        path[1024];
    int         width, height;
    long long   duration_us;
    const char *mime;                // static "video/avc" | "video/hevc"
    float       fps;                 // nominal frame rate, 0 if unknown

    void       *asset;               // CFBridgingRetain'd Obj-C objects
    void       *track;
    void       *reader;
    void       *out;

    unsigned char *psets;            // parameter sets as Annex-B, prepended on keyframes
    size_t         psets_len;
    int            nal_len_size;     // AVCC length-prefix width (usually 4)

    unsigned char *cur;              // current sample, Annex-B
    size_t         cur_len;
    long long      cur_pts_us;
    int            cur_key;
    int            eos;

    // The SOUND of the clip, when it has any: its first audio track decoded by
    // AVFoundation to 16-bit interleaved PCM and handed out in chunks of
    // KLAD_AUDIO_CHUNK frames, as an "audio/raw" track (kl_mediandk.c). The
    // reader starts on first use, so a guest that never selects it pays nothing.
    void          *atrack, *areader, *aout;
    int            arate, ach;
    int            astarted, aeos;
    unsigned char *apcm;             // the current decoded sample buffer
    size_t         apcm_len, apos;
    long long      apcm_pts_us;      // presentation time of apcm[0]
};

#define KLAD_AUDIO_CHUNK 1024        // frames per sample handed to the guest

static const uint8_t k_startcode[4] = { 0, 0, 0, 1 };

// Pull the format description's parameter sets into one Annex-B blob, and learn
// the NAL length-prefix width. Returns 0 on failure.
static int demux_build_psets(kl_avdemux *m, CMFormatDescriptionRef fmt, int hevc) {
    size_t total = 0, count = 0;
    int nalLen = 4;
    // First pass: index 0 tells us the count and the header length.
    const uint8_t *p = NULL; size_t sz = 0;
    OSStatus st = hevc
        ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, 0, &p, &sz, &count, &nalLen)
        : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, &p, &sz, &count, &nalLen);
    if (st != noErr || count == 0) return 0;
    m->nal_len_size = nalLen > 0 ? nalLen : 4;
    for (size_t i = 0; i < count; i++) {
        st = hevc
            ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, i, &p, &sz, NULL, NULL)
            : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &p, &sz, NULL, NULL);
        if (st != noErr) return 0;
        total += 4 + sz;
    }
    m->psets = malloc(total);
    if (!m->psets) return 0;
    size_t o = 0;
    for (size_t i = 0; i < count; i++) {
        st = hevc
            ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, i, &p, &sz, NULL, NULL)
            : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &p, &sz, NULL, NULL);
        if (st != noErr) { free(m->psets); m->psets = NULL; return 0; }
        memcpy(m->psets + o, k_startcode, 4); o += 4;
        memcpy(m->psets + o, p, sz);          o += sz;
    }
    m->psets_len = total;
    return 1;
}

// Convert one AVCC sample buffer to Annex-B, prepending parameter sets when it
// is a keyframe. Stores the result in m->cur.
static int demux_convert(kl_avdemux *m, CMSampleBufferRef sb) {
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    if (!bb) return 0;
    size_t avcc_len = CMBlockBufferGetDataLength(bb);
    unsigned char *avcc = malloc(avcc_len ? avcc_len : 1);
    if (!avcc) return 0;
    if (CMBlockBufferCopyDataBytes(bb, 0, avcc_len, avcc) != noErr) { free(avcc); return 0; }

    // Keyframe? kCMSampleAttachmentKey_NotSync absent or false means "sync".
    int key = 1;
    CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (atts && CFArrayGetCount(atts) > 0) {
        CFDictionaryRef d0 = CFArrayGetValueAtIndex(atts, 0);
        CFBooleanRef notSync = NULL;
        if (d0 && CFDictionaryGetValueIfPresent(d0, kCMSampleAttachmentKey_NotSync,
                                                (const void **)&notSync)
               && notSync && CFBooleanGetValue(notSync))
            key = 0;
    }

    // Worst-case output: psets (if key) + every NAL grows by (4 - nal_len_size),
    // which is 0 for the usual 4-byte prefix. Size it exactly by walking once.
    int nls = m->nal_len_size;
    size_t body = 0, pos = 0;
    while (pos + (size_t)nls <= avcc_len) {
        uint32_t l = 0;
        for (int i = 0; i < nls; i++) l = (l << 8) | avcc[pos + i];
        pos += nls;
        if (l == 0 || pos + l > avcc_len) break;   // malformed; stop cleanly
        body += 4 + l;
        pos += l;
    }
    size_t out_len = body + (key ? m->psets_len : 0);
    unsigned char *out = malloc(out_len ? out_len : 1);
    if (!out) { free(avcc); return 0; }
    size_t o = 0;
    if (key && m->psets_len) { memcpy(out, m->psets, m->psets_len); o = m->psets_len; }
    pos = 0;
    while (pos + (size_t)nls <= avcc_len) {
        uint32_t l = 0;
        for (int i = 0; i < nls; i++) l = (l << 8) | avcc[pos + i];
        pos += nls;
        if (l == 0 || pos + l > avcc_len) break;
        memcpy(out + o, k_startcode, 4); o += 4;
        memcpy(out + o, avcc + pos, l);  o += l;
        pos += l;
    }
    free(avcc);

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
    free(m->cur);
    m->cur = out;
    m->cur_len = o;
    m->cur_pts_us = CMTIME_IS_VALID(pts) ? (long long)(CMTimeGetSeconds(pts) * 1e6) : 0;
    m->cur_key = key;
    return 1;
}

// Pull the next sample into m->cur; sets m->eos at end of stream.
static void demux_pull(kl_avdemux *m) {
    AVAssetReaderTrackOutput *out = (__bridge AVAssetReaderTrackOutput *)m->out;
    for (;;) {
        CMSampleBufferRef sb = [out copyNextSampleBuffer];
        if (!sb) { m->eos = 1; free(m->cur); m->cur = NULL; m->cur_len = 0; return; }
        int ok = demux_convert(m, sb);
        CFRelease(sb);
        if (ok) return;                 // a sample with no data buffer is skipped
    }
}

static int demux_start_reader(kl_avdemux *m, CMTime from) {
    AVAsset *asset = (__bridge AVAsset *)m->asset;
    AVAssetTrack *track = (__bridge AVAssetTrack *)m->track;
    NSError *err = nil;
    AVAssetReader *r = [AVAssetReader assetReaderWithAsset:asset error:&err];
    if (!r) return 0;
    // nil outputSettings: hand back the COMPRESSED samples, not decoded pixels.
    AVAssetReaderTrackOutput *o =
        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
    o.alwaysCopiesSampleData = NO;
    if (![r canAddOutput:o]) return 0;
    [r addOutput:o];
    if (CMTIME_IS_VALID(from) && CMTimeGetSeconds(from) > 0)
        r.timeRange = CMTimeRangeMake(from, kCMTimePositiveInfinity);
    if (![r startReading]) return 0;
    if (m->reader) CFBridgingRelease(m->reader);
    if (m->out)    CFBridgingRelease(m->out);
    m->reader = (void *)CFBridgingRetain(r);
    m->out    = (void *)CFBridgingRetain(o);
    return 1;
}

kl_avdemux *kl_avdemux_open(const char *container, long long offset, long long size) {
    if (!container || !*container) return NULL;
    char real[1024];
    if (offset > 0 || size > 0) {
        if (!slice_out(container, offset, size, real, sizeof real)) return NULL;
    } else {
        snprintf(real, sizeof real, "%s", container);
    }
    kl_avdemux *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    snprintf(m->path, sizeof m->path, "%s", real);
    m->nal_len_size = 4;

    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:real]];
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
        AVAssetTrack *t = first_video_track(asset);
        if (!t) {
            fprintf(stderr, "  [avdemux] %s carries no video track\n", real);
            free(m); return NULL;
        }
        CMFormatDescriptionRef fmt =
            (__bridge CMFormatDescriptionRef)t.formatDescriptions.firstObject;
        if (!fmt) { free(m); return NULL; }
        FourCharCode sub = CMFormatDescriptionGetMediaSubType(fmt);
        int hevc = (sub == kCMVideoCodecType_HEVC);
        m->mime = hevc ? "video/hevc" : "video/avc";
        if (!demux_build_psets(m, fmt, hevc)) {
            fprintf(stderr, "  [avdemux] %s: could not read parameter sets\n", real);
            free(m); return NULL;
        }
        CGSize sz = t.naturalSize;
        m->width = (int)sz.width; m->height = (int)sz.height;
        m->duration_us = (long long)(CMTimeGetSeconds(asset.duration) * 1e6);
        m->fps = t.nominalFrameRate;
        m->asset = (void *)CFBridgingRetain(asset);
        m->track = (void *)CFBridgingRetain(t);
        AVAssetTrack *at = first_track_of(asset, AVMediaTypeAudio);
        CMAudioFormatDescriptionRef afmt = at
            ? (__bridge CMAudioFormatDescriptionRef)at.formatDescriptions.firstObject : NULL;
        const AudioStreamBasicDescription *asbd =
            afmt ? CMAudioFormatDescriptionGetStreamBasicDescription(afmt) : NULL;
        if (asbd && asbd->mSampleRate > 0 && asbd->mChannelsPerFrame > 0) {
            m->arate = (int)(asbd->mSampleRate + 0.5);
            // Down to stereo: a guest that mixes the clip in plays it in a
            // stereo mixer, and AVFoundation folds the rest in properly.
            m->ach = asbd->mChannelsPerFrame > 2 ? 2 : (int)asbd->mChannelsPerFrame;
            m->atrack = (void *)CFBridgingRetain(at);
        }
        if (!demux_start_reader(m, kCMTimeZero)) {
            fprintf(stderr, "  [avdemux] %s: reader would not start\n", real);
            kl_avdemux_close(m); return NULL;
        }
    }
    demux_pull(m);      // make the first sample current
    fprintf(stderr, "  [avdemux] %s: %s %dx%d, %lld us%s\n",
            m->path, m->mime, m->width, m->height, m->duration_us,
            m->atrack ? ", with sound" : "");
    if (m->atrack)
        fprintf(stderr, "  [avdemux]   sound: %d Hz, %d channel(s), offered as audio/raw\n",
                m->arate, m->ach);
    return m;
}

void kl_avdemux_close(kl_avdemux *m) {
    if (!m) return;
    if (m->out)    CFBridgingRelease(m->out);
    if (m->reader) CFBridgingRelease(m->reader);
    if (m->track)  CFBridgingRelease(m->track);
    if (m->aout)    CFBridgingRelease(m->aout);
    if (m->areader) CFBridgingRelease(m->areader);
    if (m->atrack)  CFBridgingRelease(m->atrack);
    if (m->asset)  CFBridgingRelease(m->asset);
    free(m->psets);
    free(m->cur);
    free(m->apcm);
    free(m);
}

int kl_avdemux_info(kl_avdemux *m, int *w, int *h, const char **mime, long long *dur,
                    float *fps) {
    if (!m) return 0;
    if (w)    *w = m->width;
    if (h)    *h = m->height;
    if (mime) *mime = m->mime;
    if (dur)  *dur = m->duration_us;
    if (fps)  *fps = m->fps;
    return 1;
}

long long kl_avdemux_sample_time_us(kl_avdemux *m) {
    return (m && !m->eos) ? m->cur_pts_us : -1;
}

int kl_avdemux_sample_keyframe(kl_avdemux *m) {
    return (m && !m->eos) ? m->cur_key : 0;
}

long kl_avdemux_read(kl_avdemux *m, unsigned char *buf, unsigned long cap) {
    if (!m || m->eos || !m->cur) return -1;
    if (buf && cap) memcpy(buf, m->cur, m->cur_len < cap ? m->cur_len : cap);
    return (long)m->cur_len;          // full size, AMediaExtractor convention
}

int kl_avdemux_advance(kl_avdemux *m) {
    if (!m || m->eos) return 0;
    demux_pull(m);
    return m->eos ? 0 : 1;
}

int kl_avdemux_seek_us(kl_avdemux *m, long long us) {
    if (!m) return 0;
    m->eos = 0;
    if (!demux_start_reader(m, CMTimeMake(us, 1000000))) return 0;
    demux_pull(m);
    return m->eos ? 0 : 1;
}

// ---------------------------------------------------------------------------
// The sound track — see the fields in struct kl_avdemux.

static int audio_start_reader(kl_avdemux *m, CMTime from) {
    AVAsset *asset = (__bridge AVAsset *)m->asset;
    AVAssetTrack *track = (__bridge AVAssetTrack *)m->atrack;
    NSError *err = nil;
    AVAssetReader *r = [AVAssetReader assetReaderWithAsset:asset error:&err];
    if (!r) return 0;
    NSDictionary *pcm = @{
        AVFormatIDKey:               @(kAudioFormatLinearPCM),
        AVLinearPCMBitDepthKey:      @16,
        AVLinearPCMIsFloatKey:       @NO,
        AVLinearPCMIsBigEndianKey:   @NO,
        AVLinearPCMIsNonInterleaved: @NO,
        AVSampleRateKey:             @(m->arate),
        AVNumberOfChannelsKey:       @(m->ach),
    };
    AVAssetReaderTrackOutput *o =
        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:pcm];
    o.alwaysCopiesSampleData = NO;
    if (![r canAddOutput:o]) return 0;
    [r addOutput:o];
    if (CMTIME_IS_VALID(from) && CMTimeGetSeconds(from) > 0)
        r.timeRange = CMTimeRangeMake(from, kCMTimePositiveInfinity);
    if (![r startReading]) return 0;
    if (m->areader) CFBridgingRelease(m->areader);
    if (m->aout)    CFBridgingRelease(m->aout);
    m->areader = (void *)CFBridgingRetain(r);
    m->aout    = (void *)CFBridgingRetain(o);
    return 1;
}

// The next decoded buffer into apcm; aeos at the end.
static void audio_pull(kl_avdemux *m) {
    AVAssetReaderTrackOutput *out = (__bridge AVAssetReaderTrackOutput *)m->aout;
    free(m->apcm); m->apcm = NULL; m->apcm_len = 0; m->apos = 0;
    for (;;) {
        CMSampleBufferRef sb = out ? [out copyNextSampleBuffer] : NULL;
        if (!sb) { m->aeos = 1; return; }
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
        size_t n = bb ? CMBlockBufferGetDataLength(bb) : 0;
        unsigned char *p = n ? malloc(n) : NULL;
        if (p && CMBlockBufferCopyDataBytes(bb, 0, n, p) == noErr) {
            CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
            m->apcm = p; m->apcm_len = n;
            m->apcm_pts_us = CMTIME_IS_VALID(pts) ? (long long)(CMTimeGetSeconds(pts) * 1e6) : 0;
            CFRelease(sb);
            return;
        }
        free(p);
        CFRelease(sb);
    }
}

static int audio_ready(kl_avdemux *m) {
    if (!m || !m->atrack) return 0;
    if (!m->astarted) {
        m->astarted = 1;
        @autoreleasepool {
            if (!audio_start_reader(m, kCMTimeZero)) {
                fprintf(stderr, "  [avdemux] %s: the sound reader would not start\n", m->path);
                m->aeos = 1;
                return 0;
            }
            audio_pull(m);
        }
    }
    return !m->aeos;
}

static size_t audio_chunk(const kl_avdemux *m) {
    size_t frame = (size_t)m->ach * 2;
    size_t left = m->apcm_len - m->apos;
    size_t want = (size_t)KLAD_AUDIO_CHUNK * frame;
    return left < want ? left : want;
}

int kl_avdemux_audio_info(kl_avdemux *m, int *rate, int *channels) {
    if (!m || !m->atrack) return 0;
    if (rate)     *rate = m->arate;
    if (channels) *channels = m->ach;
    return 1;
}

long long kl_avdemux_audio_time_us(kl_avdemux *m) {
    if (!audio_ready(m)) return -1;
    long long frames = (long long)(m->apos / ((size_t)m->ach * 2));
    return m->apcm_pts_us + frames * 1000000LL / m->arate;
}

long kl_avdemux_audio_read(kl_avdemux *m, unsigned char *buf, unsigned long cap) {
    if (!audio_ready(m) || !m->apcm) return -1;
    size_t n = audio_chunk(m);
    if (buf && cap) memcpy(buf, m->apcm + m->apos, n < cap ? n : cap);
    return (long)n;
}

int kl_avdemux_audio_advance(kl_avdemux *m) {
    if (!audio_ready(m)) return 0;
    m->apos += audio_chunk(m);
    if (m->apos >= m->apcm_len) {
        @autoreleasepool { audio_pull(m); }
    }
    return m->aeos ? 0 : 1;
}

int kl_avdemux_audio_seek_us(kl_avdemux *m, long long us) {
    if (!m || !m->atrack) return 0;
    m->astarted = 1;
    m->aeos = 0;
    @autoreleasepool {
        if (!audio_start_reader(m, CMTimeMake(us, 1000000))) { m->aeos = 1; return 0; }
        audio_pull(m);
    }
    return m->aeos ? 0 : 1;
}
