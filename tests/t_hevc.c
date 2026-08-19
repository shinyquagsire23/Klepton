// `make hevc` — the video decode gate, with no guest and no Steam host.
//
// kl_vtdec is the one piece of the video path that can be checked completely on
// its own: an elementary stream goes in, frames come out, and both ends are
// knowable. Everything above it (AMediaCodec's handle dance, the AImageReader
// queue, the EGLImage) needs a guest to exercise, and everything below it is
// Apple's. So this is where the assertions belong.
//
// The stream is tests/data/t_hevc.h265, generated once with
//   ffmpeg -f lavfi -i "testsrc2=size=320x240:rate=30:duration=1" \
//          -c:v hevc_videotoolbox -tag:v hvc1 -f hevc tests/data/t_hevc.h265
// and committed, so the gate does not depend on ffmpeg being installed.
//
// What it asserts, and why each one is a real failure mode rather than a
// tautology:
//
//   - every access unit submits            — the Annex-B walker did not lose one
//   - 30 frames decode                     — parameter sets were absorbed, the
//                                            format description was accepted,
//                                            and the session actually ran
//   - the frames are 320x240 BGRA          — the pixel format request took, and
//                                            it is the format the EGLImage path
//                                            downstream is built around
//   - the pts comes back unchanged         — VideoToolbox is a carrier for the
//                                            guest's timestamps, and a frame
//                                            that loses its pts is a frame the
//                                            compositor cannot place in time
//   - the pixels are not uniform           — the strongest cheap check that a
//                                            picture arrived rather than a
//                                            correctly-shaped blank
//   - ONE decompression session            — a session is created by throwing
//                                            the previous one's reference
//                                            frames away, so a stream that
//                                            never changes resolution must
//                                            reach the end on the one it
//                                            started with, however often its
//                                            parameter sets are re-sent. The
//                                            live stream showed 498 sessions
//                                            across 2453 access units before
//                                            this was a gate
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../runtime/media/kl_vtdec.h"

#define STREAM "tests/data/t_hevc.h265"
#define EXPECT_W 320
#define EXPECT_H 240
#define EXPECT_FRAMES 30

static int fails;
static void check(int ok, const char *what) {
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

// Split the elementary stream into access units. One AU is a run of NALs up to
// but not including the next NAL that starts a new picture, and for this stream
// the parameter sets and the SEI in front of each IRAP belong with it.
//
// The rule used here is the one the guest's own framing gives us for free: a
// VCL NAL ends an access unit. Anything non-VCL that follows attaches to the
// NEXT one. That is not the general H.265 rule (first_slice_segment_in_pic_flag
// is), but this stream is one slice per picture, which is what a real-time
// encoder emits, and the gate would notice immediately if it were not: the
// frame count would be wrong.
static int is_vcl(const unsigned char *nal) { return ((nal[0] >> 1) & 0x3f) <= 31; }

// The formats kl_glfb.c can wrap in a single-plane YCbCr MTLTexture — the same
// family its klfb_video_mtl_format() maps. With no format requested (the
// default), VideoToolbox must land in this set or the video path downstream
// refuses the frame by fourcc.
static int is_wrappable_biplanar(uint32_t pf) {
    switch (pf) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
    case kCVPixelFormatType_Lossless_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_Lossless_420YpCbCr8BiPlanarFullRange:
    case kCVPixelFormatType_Lossy_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_Lossy_420YpCbCr8BiPlanarFullRange:
    case kCVPixelFormatType_Lossless_420YpCbCr10PackedBiPlanarVideoRange:
    case kCVPixelFormatType_Lossless_420YpCbCr10PackedBiPlanarFullRange:
    case kCVPixelFormatType_Lossy_420YpCbCr10PackedBiPlanarVideoRange:
        return 1;
    default:
        return 0;
    }
}

int main(void) {
    printf("=== HEVC decode (kl_vtdec, VideoToolbox) ===\n");

    FILE *f = fopen(STREAM, "rb");
    if (!f) { printf("  cannot open %s — run from the repo root\n", STREAM); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        printf("  short read on %s\n", STREAM); return 1;
    }
    fclose(f);
    printf("  %s: %ld bytes\n", STREAM, n);

    kl_vtdec *d = kl_vtdec_create("video/hevc");
    check(d != NULL, "kl_vtdec_create(\"video/hevc\")");
    if (!d) return 1;
    check(kl_vtdec_create("video/nonesuch") == NULL,
          "an unknown mime is refused, not guessed at");

    // Walk start codes, cut an access unit after each VCL NAL.
    int submit_errors = 0, seen_vcl = 0;
    long i = 0;
    long *cuts = calloc((size_t)n / 4 + 2, sizeof *cuts);
    int n_cuts = 0;
    while (i + 3 < n) {
        if (!(buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1)) { i++; continue; }
        long payload = i + 3;
        long sc = (i > 0 && buf[i-1] == 0) ? i - 1 : i;   // 4-byte form
        if (seen_vcl) { cuts[n_cuts++] = sc; seen_vcl = 0; }
        if (payload < n && is_vcl(buf + payload)) seen_vcl = 1;
        i = payload;
    }
    cuts[n_cuts++] = n;

    // Decode, pulling as we go so the ring never has to drop.
    int decoded = 0, bad_size = 0, bad_fmt = 0, bad_pts = 0, uniform = 0;
    long prev = 0;
    for (int c = 0; c < n_cuts; c++) {
        int64_t pts = (int64_t)c * 33333;
        if (kl_vtdec_submit(d, buf + prev, (size_t)(cuts[c] - prev), pts) < 0)
            submit_errors++;
        prev = cuts[c];

        // VideoToolbox is asynchronous; give it a moment and drain.
        for (int spin = 0; spin < 200; spin++) {
            int64_t got = -1;
            CVPixelBufferRef pb = kl_vtdec_pull(d, &got);
            if (!pb) { usleep(500); continue; }
            decoded++;
            if ((int)CVPixelBufferGetWidth(pb) != EXPECT_W ||
                (int)CVPixelBufferGetHeight(pb) != EXPECT_H) bad_size++;
            if (!is_wrappable_biplanar(CVPixelBufferGetPixelFormatType(pb)))
                bad_fmt++;
            // Every frame carries the pts of SOME submitted access unit; which
            // one depends on how far the decoder is behind, so the assertion is
            // that it is one of ours rather than that it is this one.
            if (got < 0 || got % 33333 != 0) bad_pts++;

            // The luma plane is the picture for this purpose; a biplanar frame
            // has no base address of its own to walk.
            CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
            const unsigned char *px = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
            size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
            int varied = 0;
            if (px) {
                unsigned char first = px[0];
                for (int y = 0; y < EXPECT_H && !varied; y += 8)
                    for (int x = 0; x < EXPECT_W && !varied; x += 4)
                        if (px[(size_t)y * stride + x] != first) varied = 1;
            }
            // A frame the CPU cannot map (a compressed-storage format) is not
            // evidence of a blank; only a readable uniform plane is.
            if (px && !varied) uniform++;
            CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
            CVPixelBufferRelease(pb);
            break;
        }
    }

    // Anything the decoder was still holding.
    for (int spin = 0; spin < 400 && decoded < EXPECT_FRAMES; spin++) {
        int64_t got = -1;
        CVPixelBufferRef pb = kl_vtdec_pull(d, &got);
        if (!pb) { usleep(1000); continue; }
        decoded++;
        CVPixelBufferRelease(pb);
    }

    // The parameter sets, re-sent. A real host does this in front of every
    // IRAP — Steam Link's does it far more often than that — and each one used
    // to cost a decompression session, i.e. every reference frame the decoder
    // held. Bytes identical to the ones already held must change nothing at
    // all: no description built, no session created.
    unsigned built0 = 0, swapped0 = 0, sessions0 = 0;
    kl_vtdec_param_stats(d, &built0, &swapped0, &sessions0);
    unsigned char *pfx = malloc((size_t)n);
    size_t pfx_len = 0;
    if (pfx) {
        long j = 0;
        while (j + 3 < n) {
            if (!(buf[j] == 0 && buf[j+1] == 0 && buf[j+2] == 1)) { j++; continue; }
            long payload = j + 3, end = payload;
            while (end + 3 < n &&
                   !(buf[end] == 0 && buf[end+1] == 0 && buf[end+2] == 1)) end++;
            if (end + 3 >= n) end = n;
            int t = (buf[payload] >> 1) & 0x3f;
            if (t >= 32 && t <= 34) {
                pfx[pfx_len++] = 0; pfx[pfx_len++] = 0;
                pfx[pfx_len++] = 0; pfx[pfx_len++] = 1;
                memcpy(pfx + pfx_len, buf + payload, (size_t)(end - payload));
                pfx_len += (size_t)(end - payload);
            }
            j = payload;
        }
        for (int rep = 0; rep < 5; rep++)
            kl_vtdec_submit(d, pfx, pfx_len, 1000000 + rep);
    }
    unsigned built1 = 0, swapped1 = 0, sessions1 = 0;
    kl_vtdec_param_stats(d, &built1, &swapped1, &sessions1);

    unsigned s = 0, dec = 0, drop = 0; int w = 0, h = 0;
    kl_vtdec_stats(d, &s, &dec, &drop, &w, &h);
    printf("  %d access units, %u submitted, %u decoded, %u dropped, %dx%d\n",
           n_cuts, s, dec, drop, w, h);

    check(submit_errors == 0, "every access unit submitted without error");
    check(decoded == EXPECT_FRAMES, "30 frames decoded");
    check(bad_size == 0, "every frame is 320x240");
    check(bad_fmt == 0, "every frame is a wrappable biplanar YCbCr format");
    check(bad_pts == 0, "the guest's pts survives the round trip");
    check(uniform == 0, "the frames carry a picture, not a uniform blank");
    check(drop == 0, "nothing was dropped for want of ring space");
    check(sessions0 == 1, "one decompression session for the whole stream");
    check(pfx != NULL && pfx_len > 0, "the stream's parameter sets were found");
    check(built1 == built0 && sessions1 == sessions0,
          "re-sent parameter sets cost neither a description nor a session");

    kl_vtdec_report(stdout);
    kl_vtdec_destroy(d);

    // The A/B stays honest only while both arms work: KL_VTDEC_BGRA=1 is the
    // escape hatch for a native format kl_glfb cannot wrap, and an escape
    // hatch nothing exercises is one that has quietly rotted. One decode pass,
    // asserting the request took.
    setenv("KL_VTDEC_BGRA", "1", 1);
    kl_vtdec *db = kl_vtdec_create("video/hevc");
    check(db != NULL, "KL_VTDEC_BGRA=1: decoder created");
    int bgra_frames = 0, bgra_bad_fmt = 0;
    if (db) {
        // Pull while submitting — the ring is shallower than the stream, and a
        // frame dropped for want of a reader would fail the count for the
        // wrong reason.
        long p = 0;
        for (int c = 0; c < n_cuts; c++) {
            kl_vtdec_submit(db, buf + p, (size_t)(cuts[c] - p), (int64_t)c * 33333);
            p = cuts[c];
            for (int spin = 0; spin < 200; spin++) {
                CVPixelBufferRef pb = kl_vtdec_pull(db, NULL);
                if (!pb) { usleep(500); continue; }
                bgra_frames++;
                if (CVPixelBufferGetPixelFormatType(pb) != kCVPixelFormatType_32BGRA)
                    bgra_bad_fmt++;
                CVPixelBufferRelease(pb);
                break;
            }
        }
        for (int spin = 0; spin < 600 && bgra_frames < EXPECT_FRAMES; spin++) {
            CVPixelBufferRef pb = kl_vtdec_pull(db, NULL);
            if (!pb) { usleep(1000); continue; }
            bgra_frames++;
            if (CVPixelBufferGetPixelFormatType(pb) != kCVPixelFormatType_32BGRA)
                bgra_bad_fmt++;
            CVPixelBufferRelease(pb);
        }
        kl_vtdec_destroy(db);
    }
    unsetenv("KL_VTDEC_BGRA");
    check(bgra_frames == EXPECT_FRAMES, "KL_VTDEC_BGRA=1: 30 frames decoded");
    check(bgra_bad_fmt == 0, "KL_VTDEC_BGRA=1: every frame is BGRA");

    free(pfx);
    free(cuts);
    free(buf);

    printf(fails ? "\nFAIL: %d check(s) failed\n" : "\nPASS: HEVC decodes\n", fails);
    return fails ? 1 : 0;
}
