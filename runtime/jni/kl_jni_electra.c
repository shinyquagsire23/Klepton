// Unreal Engine 5: com.epicgames.unreal.ElectraVideoDecoderH264
//
// UE5's Electra media plugin, Android backend. Where UE4 shipped MediaPlayer14
// (a whole player, see kl_jni_ue4.c), UE5's Electra splits the job: the engine
// demuxes the MP4 and drives a bare H.264 DECODER through a MediaCodec-shaped
// Java class. Wanderer's intro video reaches exactly this class, and the guest
// calls it in a fixed order — CreateDecoder, ConfigureDecoder, SetOutputSurface,
// Start, then the per-access-unit loop QueueInputBuffer / DequeueOutputBuffer /
// GetOutputBuffer.
//
// The contract is transcribed from the guest's own GetMethodID / GetFieldID
// traffic (klepton-wanderer.log around the FindClass for this class), not from a
// version of Epic's sources — the ids it resolves ARE the contract. It is a
// MediaCodec wearing Electra's names: DequeueInputBuffer hands out an index,
// QueueInputBuffer feeds an access unit at that index, DequeueOutputBuffer says
// whether a frame is ready and at which output index, GetOutputBuffer copies
// that frame out as a byte[].
//
// The decoder is kl_vtdec (VideoToolbox), the same host device UE4's cutscene
// path uses. kl_vtdec is shaped for precisely this — "here is an access unit,
// give me back a frame" — so this file is only the contract and the two format
// conversions the seam needs: the guest's Annex-B/AVCC in, and NV12 out.
//
// See runtime/jni/kl_jni_int.h for the seam and kl_jni_ue4.c's MediaPlayer14 for
// the same pattern at one more remove.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kl_jni.h"
#include "kl_vtdec.h"   // pulls in <CoreVideo/CoreVideo.h>
#include "kl_jni_int.h"

// Spelled in the ue4 package: the JNI surface folds every
// `com/epicgames/unreal/` class onto `com/epicgames/ue4/` at its intern funnel
// (kl_jni.c, klj_fold_class_name), so the class the guest asks for as
// com/epicgames/unreal/ElectraVideoDecoderH264 is interned under this name.
#define KLE_H264_CLASS  "com/epicgames/ue4/ElectraVideoDecoderH264"
#define KLE_H265_CLASS  "com/epicgames/ue4/ElectraVideoDecoderH265"

// UE5's Electra registers a decoder class per codec — H264 and H265 — and the
// engine probes BOTH at startup (GetDecoderInformation) to enumerate codec
// support before it picks one. Serving only H264 fatals the run the instant it
// asks H265 for its capabilities (kl_jni aborts on a called method with no host
// implementation). The two classes are the same MediaCodec seam with different
// nested-class names and a different kl_vtdec mime; one set of handlers drives
// both, keyed on the instance's (or, for a constructor, the class's) own name.
// kl_vtdec decodes both video/avc and video/hevc through VideoToolbox, and the
// Annex-B/AVCC framing this file converts is identical for the two codecs.
typedef struct {
    const char *cls;      // decoder class
    const char *params;   // $FCreateParameters
    const char *decinfo;  // $FDecoderInformation
    const char *outfmt;   // $FOutputFormatInfo
    const char *outbuf;   // $FOutputBufferInfo
    const char *mime;     // kl_vtdec mime
} kle_codec;

static const kle_codec KLE_CODEC_H264 = {
    KLE_H264_CLASS, KLE_H264_CLASS "$FCreateParameters",
    KLE_H264_CLASS "$FDecoderInformation", KLE_H264_CLASS "$FOutputFormatInfo",
    KLE_H264_CLASS "$FOutputBufferInfo", "video/avc",
};
static const kle_codec KLE_CODEC_H265 = {
    KLE_H265_CLASS, KLE_H265_CLASS "$FCreateParameters",
    KLE_H265_CLASS "$FDecoderInformation", KLE_H265_CLASS "$FOutputFormatInfo",
    KLE_H265_CLASS "$FOutputBufferInfo", "video/hevc",
};

static const kle_codec *kle_codec_by_class(const char *cls) {
    return (cls && strcmp(cls, KLE_H265_CLASS) == 0) ? &KLE_CODEC_H265
                                                     : &KLE_CODEC_H264;
}

// COLOR_FormatYUV420SemiPlanar — the MediaCodec constant for NV12, which is what
// GetOutputBuffer packs. The guest reads FOutputFormatInfo.ColorFormat and
// unpacks accordingly, so the two must agree.
#define KLE_COLOR_NV12  21

// A frame decoded but not yet handed out. The guest dequeues an output index,
// then reads the format and the bytes for that index, then releases it — so a
// small ring of pixel buffers keyed by index sits between DequeueOutputBuffer
// and ReleaseOutputBuffer. 8 is far more than the one-frame-at-a-time the guest
// actually keeps live.
#define KLE_SLOTS 8

typedef struct {
    const kle_codec *codec;        // which decoder class this instance is
    kl_vtdec        *dec;
    CVPixelBufferRef slots[KLE_SLOTS];
    int              next_slot;
    int              started;
    int              eos;          // QueueEOSInputBuffer seen
    int              fmt_warned;   // GetOutputBuffer's unexpected-format log-once
} kl_electra;

static kl_electra *kle_of(void *self) {
    klj_object *o = klj_as_object(self);
    if (!o) return NULL;
    if (strcmp(o->cls, KLE_H264_CLASS) && strcmp(o->cls, KLE_H265_CLASS))
        return NULL;
    return o->data;
}

// The codec for a handler whose `self` is a decoder instance (every method but
// the two constructors, which get the class object instead). Prefer the payload
// this file stamped at construction, but fall back to the object's own class
// name so a return object is still namespaced correctly even if the instance
// reached us without going through our ctor.
static const kle_codec *kle_codec_of(void *self) {
    kl_electra *e = kle_of(self);
    if (e && e->codec) return e->codec;
    klj_object *o = klj_as_object(self);
    return kle_codec_by_class(o ? o->cls : NULL);
}

// ---- format in: the guest's bitstream -> Annex-B ---------------------------
//
// kl_vtdec wants Annex-B (00 00 00 01 start codes). Electra's CSD and frame
// data may arrive already Annex-B, OR as AVCC (4-byte big-endian length prefix
// per NAL). Detect and convert; a start code at the front means Annex-B and is
// passed through untouched.
static int kle_is_annexb(const uint8_t *p, size_t n) {
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 1;
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 1;
    return 0;
}

// AVCC (4-byte length-prefixed NALs) -> Annex-B, into a fresh buffer the caller
// frees. Each 4-byte length becomes a 4-byte start code, so the output is the
// same size as the input. Returns 0 on success. A prefix that runs past the end
// (or a zero length) means this was not AVCC after all — reported by a negative
// return so the caller can fall back rather than corrupt the stream.
static int kle_avcc_to_annexb(const uint8_t *in, size_t len,
                              uint8_t **out, size_t *outlen) {
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) return -1;
    size_t ip = 0, op = 0;
    while (ip + 4 <= len) {
        uint32_t nal = ((uint32_t)in[ip] << 24) | ((uint32_t)in[ip + 1] << 16) |
                       ((uint32_t)in[ip + 2] << 8) | (uint32_t)in[ip + 3];
        ip += 4;
        if (nal == 0 || ip + nal > len) { free(buf); return -1; }
        buf[op++] = 0; buf[op++] = 0; buf[op++] = 0; buf[op++] = 1;
        memcpy(buf + op, in + ip, nal);
        op += nal; ip += nal;
    }
    if (ip != len) { free(buf); return -1; }
    *out = buf; *outlen = op;
    return 0;
}

static void kle_submit(kl_electra *e, const uint8_t *data, size_t len,
                       int64_t pts_us) {
    if (!e || !e->dec || !data || !len) return;
    if (kle_is_annexb(data, len)) {
        kl_vtdec_submit(e->dec, data, len, pts_us);
        return;
    }
    uint8_t *ab = NULL; size_t abl = 0;
    if (kle_avcc_to_annexb(data, len, &ab, &abl) == 0) {
        kl_vtdec_submit(e->dec, ab, abl, pts_us);
        free(ab);
    } else {
        // Neither a recognisable start code nor a clean AVCC framing. Submit as
        // is — kl_vtdec's own NAL scan is the last word on whether it is usable.
        kl_vtdec_submit(e->dec, data, len, pts_us);
    }
}

// A byte[] argument arrives as a klj_array in a[idx].l.
static const uint8_t *kle_bytes(const klj_val *a, int idx, int n, size_t *len) {
    *len = 0;
    if (idx >= n) return NULL;
    klj_array *arr = klj_arr(a[idx].l);
    if (!arr || arr->kind != 'B') return NULL;
    *len = (size_t)arr->len;
    return arr->data;
}

// Store one primitive field on an object the guest will read back with
// Get{Int,Long,Boolean}Field. All three read the .j half (see klj_field_value in
// kl_jni.c), so one setter serves them. The jfieldID is resolved by identity —
// klj_want dedupes, so calling this per frame does not grow the id table.
static void kle_set(void *obj, const char *clsname, const char *field,
                    const char *sig, int64_t v) {
    void *cls = klj_class_object(clsname);
    klj_field_store(obj, klj_want(cls, field, sig, 'f'), (klj_val){.j = (uint64_t)v});
}

// ---- lifecycle -------------------------------------------------------------
static klj_val klj_Electra_ctor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    // A constructor is dispatched with the class object as `self`.
    const kle_codec *c = kle_codec_by_class(klj_class_name(self));
    kl_electra *e = calloc(1, sizeof *e);
    if (!e) return (klj_val){.l = NULL};
    e->codec = c;
    KLJ_LOG("%s: new decoder", c->cls);
    return (klj_val){.l = klj_new_object_data(c->cls, e)};
}

// The kl_vtdec is created here; ConfigureDecoder's parameter sets (and the
// per-frame QueueCSDInputBuffer) are absorbed by submit, so there is nothing to
// configure the session with up front.
static klj_val klj_Electra_create(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e && !e->dec) {
        e->dec = kl_vtdec_create(e->codec->mime);
        KLJ_LOG("%s.CreateDecoder -> %s", e->codec->cls,
                e->dec ? "ok" : "no VideoToolbox decoder for that mime");
    }
    return (klj_val){.j = 0};
}

// ConfigureDecoder(FCreateParameters). The parameters carry Width/Height and
// CSD0/CSD1, but Klepton cannot read instance fields the guest wrote (the write
// table is private to kl_jni.c) and does not need to: the CSDs also arrive
// through QueueCSDInputBuffer, and kl_vtdec builds its format description from
// the SPS/PPS in the stream either way. Ensure the decoder exists and return ok.
static klj_val klj_Electra_configure(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e && !e->dec) e->dec = kl_vtdec_create(e->codec->mime);
    return (klj_val){.j = 0};
}

// We output frames as byte[] buffers, not to a Surface. Ignore the surface.
static klj_val klj_Electra_setSurface(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

static klj_val klj_Electra_start(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e) { e->started = 1; e->eos = 0; }
    return (klj_val){.j = 0};
}
static klj_val klj_Electra_stop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e) e->started = 0;
    return (klj_val){.j = 0};
}

static void kle_drop_slots(kl_electra *e) {
    for (int i = 0; i < KLE_SLOTS; i++)
        if (e->slots[i]) { CVPixelBufferRelease(e->slots[i]); e->slots[i] = NULL; }
    e->next_slot = 0;
}

static klj_val klj_Electra_flush(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e) { if (e->dec) kl_vtdec_flush(e->dec); kle_drop_slots(e); e->eos = 0; }
    return (klj_val){.j = 0};
}
static klj_val klj_Electra_reset(void *env, void *self, const klj_val *a, int n) {
    return klj_Electra_flush(env, self, a, n);
}

static klj_val klj_Electra_release(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e) {
        kle_drop_slots(e);
        if (e->dec) { kl_vtdec_destroy(e->dec); e->dec = NULL; }
        e->started = 0;
    }
    return (klj_val){.j = 0};
}

// ---- input -----------------------------------------------------------------
// kl_vtdec takes bytes directly — there is no input-buffer pool to draw an index
// from — so any valid index will do, and the guest hands it straight back to
// QueueInputBuffer.
static klj_val klj_Electra_dequeueInput(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// QueueCSDInputBuffer(index, timestampUs, csd) — a codec-config buffer. Feeding
// it as an access unit is correct: kl_vtdec absorbs the parameter sets and
// treats a CSD-only submit as a no-op.
static klj_val klj_Electra_queueCSD(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kl_electra *e = kle_of(self);
    size_t len = 0;
    const uint8_t *csd = kle_bytes(a, 2, n, &len);
    kle_submit(e, csd, len, n > 1 ? (int64_t)a[1].j : 0);
    return (klj_val){.j = 0};
}

// QueueInputBuffer(index, timestampUs, data) — one access unit.
static klj_val klj_Electra_queueInput(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kl_electra *e = kle_of(self);
    size_t len = 0;
    const uint8_t *data = kle_bytes(a, 2, n, &len);
    kle_submit(e, data, len, n > 1 ? (int64_t)a[1].j : 0);
    return (klj_val){.j = 0};
}

static klj_val klj_Electra_queueEOS(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e) e->eos = 1;
    return (klj_val){.j = 0};
}

// ---- output ----------------------------------------------------------------
// DequeueOutputBuffer(timeoutMs) -> FOutputBufferInfo. Pull the oldest decoded
// frame; if one is ready, park it in a slot and report that slot as BufferIndex.
// If none, report BufferIndex = -1 (MediaCodec's INFO_TRY_AGAIN_LATER), carrying
// bIsEOS once the guest has signalled end of stream and the decoder has drained.
static klj_val klj_Electra_dequeueOutput(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    const char *ob = kle_codec_of(self)->outbuf;
    void *info = kl_jni_new_object(ob);

    int64_t pts = 0;
    CVPixelBufferRef pb = (e && e->dec) ? kl_vtdec_pull(e->dec, &pts) : NULL;
    if (pb) {
        // pts is the guest's own timestamp round-tripped through VideoToolbox.
        int slot = e->next_slot;
        if (e->slots[slot]) CVPixelBufferRelease(e->slots[slot]);
        e->slots[slot] = pb;
        e->next_slot = (slot + 1) % KLE_SLOTS;

        int w = (int)CVPixelBufferGetWidth(pb);
        int h = (int)CVPixelBufferGetHeight(pb);
        int size = w * h * 3 / 2;    // NV12
        kle_set(info, ob, "BufferIndex", "I", slot);
        kle_set(info, ob, "Size", "I", size);
        kle_set(info, ob, "PresentationTimestamp", "J", pts);
        kle_set(info, ob, "bIsEOS", "Z", 0);
        kle_set(info, ob, "bIsConfig", "Z", 0);
    } else {
        kle_set(info, ob, "BufferIndex", "I", -1);
        kle_set(info, ob, "Size", "I", 0);
        kle_set(info, ob, "PresentationTimestamp", "J", 0);
        kle_set(info, ob, "bIsEOS", "Z", (e && e->eos) ? 1 : 0);
        kle_set(info, ob, "bIsConfig", "Z", 0);
    }
    return (klj_val){.l = info};
}

static CVPixelBufferRef kle_slot(kl_electra *e, int idx) {
    if (!e || idx < 0 || idx >= KLE_SLOTS) return NULL;
    return e->slots[idx];
}

// GetOutputBuffer(index) -> byte[] in NV12 (COLOR_FormatYUV420SemiPlanar): the
// full Y plane (width*height) followed by the interleaved CbCr plane
// (width*height/2), copied out of the biplanar 4:2:0 CVPixelBuffer honouring
// each plane's own bytes-per-row.
static klj_val klj_Electra_getOutputBuffer(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kl_electra *e = kle_of(self);
    CVPixelBufferRef pb = kle_slot(e, n > 0 ? (int)a[0].j : -1);
    if (!pb) return (klj_val){.l = klj_new_array('B', NULL, 0)};

    size_t w = CVPixelBufferGetWidth(pb);
    size_t h = CVPixelBufferGetHeight(pb);
    size_t ysize = w * h;
    size_t csize = w * h / 2;
    void      *arrobj = klj_new_array('B', NULL, (int)(ysize + csize));
    klj_array *arr = klj_arr(arrobj);
    uint8_t   *dst = arr ? arr->data : NULL;
    if (!dst) return (klj_val){.l = arrobj};

    OSType fmt = CVPixelBufferGetPixelFormatType(pb);
    if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        size_t ystride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
        size_t yh      = CVPixelBufferGetHeightOfPlane(pb, 0);
        for (size_t r = 0; r < h && r < yh; r++)
            memcpy(dst + r * w, y + r * ystride, w);
        // Plane 1 is interleaved CbCr at half height; each row is w bytes
        // (w/2 chroma pairs * 2), which is exactly NV12's chroma row.
        const uint8_t *c = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        size_t cstride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
        size_t ch      = CVPixelBufferGetHeightOfPlane(pb, 1);
        uint8_t *cdst = dst + ysize;
        for (size_t r = 0; r < h / 2 && r < ch; r++)
            memcpy(cdst + r * w, c + r * cstride, w);
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    } else if (e && !e->fmt_warned) {
        e->fmt_warned = 1;
        KLJ_LOG("%s.GetOutputBuffer: pixel format 0x%x is not "
                "biplanar NV12 — returning a zero frame (needs an on-device "
                "iteration to wrap the private format)", e->codec->cls,
                (unsigned)fmt);
    }
    // Non-NV12 falls through with the calloc'd (zero) buffer, which the guest
    // renders as a black frame rather than crashing on.
    return (klj_val){.l = arrobj};
}

// GetOutputFormatInfo(index) -> FOutputFormatInfo. Dimensions come from the slot
// when it holds a frame, else from the decoder's running stats. Crops are 0 and
// the color format is NV12; Stride is the Y-plane row and SliceHeight the height.
static klj_val klj_Electra_getOutputFormat(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kl_electra *e = kle_of(self);
    const char *of = kle_codec_of(self)->outfmt;
    void *info = kl_jni_new_object(of);
    int w = 0, h = 0;
    CVPixelBufferRef pb = kle_slot(e, n > 0 ? (int)a[0].j : -1);
    if (pb) {
        w = (int)CVPixelBufferGetWidth(pb);
        h = (int)CVPixelBufferGetHeight(pb);
    } else if (e && e->dec) {
        kl_vtdec_stats(e->dec, NULL, NULL, NULL, &w, &h);
    }
    kle_set(info, of, "Width", "I", w);
    kle_set(info, of, "Height", "I", h);
    kle_set(info, of, "CropTop", "I", 0);
    kle_set(info, of, "CropBottom", "I", 0);
    kle_set(info, of, "CropLeft", "I", 0);
    kle_set(info, of, "CropRight", "I", 0);
    kle_set(info, of, "Stride", "I", w);
    kle_set(info, of, "SliceHeight", "I", h);
    kle_set(info, of, "ColorFormat", "I", KLE_COLOR_NV12);
    return (klj_val){.l = info};
}

// ReleaseOutputBuffer(index, render, ts) — free the frame at that index.
static klj_val klj_Electra_releaseOutput(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kl_electra *e = kle_of(self);
    int idx = n > 0 ? (int)a[0].j : -1;
    if (e && idx >= 0 && idx < KLE_SLOTS && e->slots[idx]) {
        CVPixelBufferRelease(e->slots[idx]);
        e->slots[idx] = NULL;
    }
    return (klj_val){.j = 0};
}

// GetDecoderInformation -> FDecoderInformation. Not adaptive, API level 29 (the
// Quest's, matching the rest of Klepton's Android surface), and no
// SetOutputSurface support — we decode to byte[] buffers, not a surface.
static klj_val klj_Electra_decoderInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    const char *di = kle_codec_of(self)->decinfo;
    void *info = kl_jni_new_object(di);
    kle_set(info, di, "bIsAdaptive", "Z", 0);
    kle_set(info, di, "ApiLevel", "I", 29);
    kle_set(info, di, "bCanUse_SetOutputSurface", "Z", 0);
    return (klj_val){.l = info};
}

// The guest news up an FCreateParameters, sets its fields, and passes it to
// ConfigureDecoder. Hand back a bare object for it to populate.
static klj_val klj_Electra_params_ctor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    // Dispatched with the FCreateParameters class object as `self`; hand back a
    // bare instance of exactly that class (H264's or H265's) for the guest to
    // populate. The fields are absorbed elsewhere, so its identity is all that
    // matters here.
    const char *cls = klj_class_name(self);
    return (klj_val){.l = kl_jni_new_object(cls ? cls : KLE_CODEC_H264.params)};
}

// One table body per codec, generated from the decoder class name C: the nested
// class names in the signatures are C with a "$..." suffix, exactly as the guest
// resolves them. Both instantiations point at the same handlers, which key their
// per-codec behaviour on the instance/class they are given.
#define KLE_TABLE(C) \
    {C, "<init>", "()V", klj_Electra_ctor}, \
    {C, "CreateDecoder", "()I", klj_Electra_create}, \
    {C, "ConfigureDecoder", "(L" C "$FCreateParameters;)I", klj_Electra_configure}, \
    {C, "SetOutputSurface", "(Landroid/view/Surface;)I", klj_Electra_setSurface}, \
    {C, "Start", "()I", klj_Electra_start}, \
    {C, "Stop", "()I", klj_Electra_stop}, \
    {C, "Flush", "()I", klj_Electra_flush}, \
    {C, "Reset", "()I", klj_Electra_reset}, \
    {C, "ReleaseDecoder", "()I", klj_Electra_release}, \
    {C, "release", "()I", klj_Electra_release}, \
    {C, "DequeueInputBuffer", "(I)I", klj_Electra_dequeueInput}, \
    {C, "QueueInputBuffer", "(IJ[B)I", klj_Electra_queueInput}, \
    {C, "QueueCSDInputBuffer", "(IJ[B)I", klj_Electra_queueCSD}, \
    {C, "QueueEOSInputBuffer", "(IJ)I", klj_Electra_queueEOS}, \
    {C, "DequeueOutputBuffer", "(I)L" C "$FOutputBufferInfo;", klj_Electra_dequeueOutput}, \
    {C, "GetOutputBuffer", "(I)[B", klj_Electra_getOutputBuffer}, \
    {C, "GetOutputFormatInfo", "(I)L" C "$FOutputFormatInfo;", klj_Electra_getOutputFormat}, \
    {C, "ReleaseOutputBuffer", "(IZJ)I", klj_Electra_releaseOutput}, \
    {C, "GetDecoderInformation", "()L" C "$FDecoderInformation;", klj_Electra_decoderInfo}, \
    {C "$FCreateParameters", "<init>", "()V", klj_Electra_params_ctor},

const klj_binding klj_bind_electra[] = {
    KLE_TABLE(KLE_H264_CLASS)
    KLE_TABLE(KLE_H265_CLASS)
    {0}
};
