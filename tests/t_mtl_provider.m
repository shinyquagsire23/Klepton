// The host-side stand-in for Compositor Services, and the gate that says
// the guest's frame really lands in an MTLTexture.
//
// On visionOS the eye textures come from the Swift side, backed by whatever the
// compositor is about to sample. There is no compositor here, so this allocates
// equivalent MTLTextures itself — same device, same format, same size — and then
// reads them back, which is the whole point: it turns "the interop primitive
// works" (`make mtltex`) into "the guest's own rendering arrives through it".
//
// Host-only and diagnostic: named by the t_boot rule alone, never in
// RUNTIME_SHIP. Objective-C because it has to *create* textures; the shipping
// runtime never does, which is why kl_glfb.c stays plain C and takes an opaque
// pointer.
//
// Enable with KL_GLFB_MTL=1 (needs KL_GLFB=1 — there is nothing to interop with
// on the null driver).
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>
#include "../runtime/kl_env.h"
#include "../runtime/gfx/kl_glfb.h"
#include "t_mtl_provider.h"

// The textures are held for the process lifetime, exactly as the contract in
// kl_glfb.h requires: Unity takes its eye textures at init and never asks again,
// so a texture released after SetupEyeTexture2 returns would leave the GL
// texture pointing at freed storage — and the failure would appear frames later
// in the compositor rather than here.
static id<MTLTexture> g_held[2][4];
static int g_slice[2][4];
static int g_dim[4][2];          // the size each stage's texture actually is
static uint32_t g_fmt[4];        // ...and the GL internalformat it was made for

// ---------------------------------------------------------------------------
// Foveation on the host (KL_VRR, on by default; KL_VRR=0 is the A/B).
//
// The host stand-in for what the visionOS compositor does: build a rate map for
// the eye size and hand it to kl_glfb, which registers it on the eye textures
// and on the guest's multisampled scene target. With it set, the guest
// rasterizes its expensive pass at a reduced rate in the periphery and the eye
// texture holds a WARPED picture — which only kl_view_mtl's composite undoes.
// A KL_GLFB_OUT capture therefore shows the warp directly, and that is the
// cheapest confirmation that any of this engaged at all.
//
// The curve here is always the synthetic one. On device the compositor samples
// the DISPLAY's own rate maps instead and re-issues that curve at the guest's
// eye size — there is no display to sample here, and a host run wants a fixed
// curve anyway so the measurement means the same thing twice.
//
// Every readback path that is not a compositor still reads the eye texture as
// if it were an ordinary image, which is deliberate — see above.
static id<MTLRasterizationRateMap> g_rate;
static int g_rate_w, g_rate_h;

// Build (or rebuild) the map for a w x h eye and push it. Called when a texture
// is allocated, so the map always matches the size the guest is rendering at —
// Unity changes that size mid-run and a stale map would foveate against the
// wrong screen size.
//
// The shape of this function is the contract KleptonCompositor.swift's
// updateGuestRateMap keeps on device; the zone count and falloff come from
// kl_glfb_foveation_wanted so the two cannot drift apart.
static void klmtl_update_rate_map(id<MTLDevice> dev, int w, int h) {
    int zones = 0;
    float edge = 0;
    if (!kl_glfb_foveation_wanted(&zones, &edge)) return;
    if (g_rate && g_rate_w == w && g_rate_h == h) return;
    if (![dev supportsRasterizationRateMapWithLayerCount:1]) {
        fprintf(stderr, "  [mtl] this GPU has no variable rasterization rate — "
                        "KL_VRR ignored\n");
        return;
    }
    float qx[KL_FOVEATION_MAX_ZONES], qy[KL_FOVEATION_MAX_ZONES];
    kl_glfb_foveation_quality(qx, zones, edge);
    kl_glfb_foveation_quality(qy, zones, edge);
    MTLRasterizationRateLayerDescriptor *layer =
        [[MTLRasterizationRateLayerDescriptor alloc]
            initWithSampleCount:MTLSizeMake((NSUInteger)zones, (NSUInteger)zones, 0)
                     horizontal:qx vertical:qy];
    MTLRasterizationRateMapDescriptor *d = [MTLRasterizationRateMapDescriptor
        rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 0)
                                               layer:layer];
    d.label = @"klepton guest foveation";
    id<MTLRasterizationRateMap> m = [dev newRasterizationRateMapWithDescriptor:d];
    if (!m) {
        fprintf(stderr, "  [mtl] newRasterizationRateMapWithDescriptor %dx%d FAILED\n", w, h);
        return;
    }
    MTLSize phys = [m physicalSizeForLayer:0];
    fprintf(stderr, "  [mtl] foveation: screen %dx%d -> physical %lux%lu "
                    "(%.1f%% of the fragments, edge rate %.2f)\n", w, h,
            (unsigned long)phys.width, (unsigned long)phys.height,
            100.0 * (double)(phys.width * phys.height) / ((double)w * (double)h), edge);
    g_rate = m; g_rate_w = w; g_rate_h = h;
    // Before the eye texture is bound: kl_glfb registers the map on each
    // texture at bind time, and ANGLE caches a framebuffer's render pass
    // descriptor, so a map that arrives later misses the first pass.
    kl_glfb_set_eye_rate_map(w, h, zones, zones, (__bridge void *)m);
}

// GL internal format -> MTLPixelFormat. ANGLE refuses the pair outright when
// they disagree ("Incompatible format"), so this is a requirement, not a
// preference: Unity asks for RGBA16F and Steam Link's OpenXR swapchains ask for
// SRGB8_ALPHA8, and one provider serves both guests.
static MTLPixelFormat klmtl_pixel_format(uint32_t gl_internal) {
    switch (gl_internal) {
    case 0x881A: return MTLPixelFormatRGBA16Float;   // GL_RGBA16F
    case 0x8C43: return MTLPixelFormatRGBA8Unorm_sRGB;  // GL_SRGB8_ALPHA8
    case 0x8058: return MTLPixelFormatRGBA8Unorm;    // GL_RGBA8
    default:     return MTLPixelFormatInvalid;
    }
}

static int provide(int eye, int stage, int w, int h, uint32_t internal_fmt,
                   kl_mtl_eye_texture *out, void *ctx) {
    (void)ctx;
    if (eye < 0 || eye > 1 || stage < 0 || stage > 3) return 0;
    MTLPixelFormat pf = klmtl_pixel_format(internal_fmt);
    if (pf == MTLPixelFormatInvalid) {
        // Declining is the right answer, not a substitute format: the guest
        // then gets ordinary GL storage of the format it asked for, which is
        // wrong in a way that shows up as a black eye rather than as colour
        // that is subtly off.
        fprintf(stderr, "  [mtl] eye %d stage %d asks for GL internalformat 0x%x, "
                        "which this provider has no MTLPixelFormat for — declining\n",
                eye, stage, internal_fmt);
        return 0;
    }
    void *devp = kl_glfb_mtl_device();
    if (!devp) {
        fprintf(stderr, "  [mtl] no MTLDevice from ANGLE — cannot provide eye textures\n");
        return 0;
    }
    // ANGLE's device, not the system default: the extension requires the texture
    // come from the display's device.
    id<MTLDevice> dev = (__bridge id<MTLDevice>)devp;

    // Reallocate when the requested size changes, because Unity DOES change it:
    // measured 1832x1920 during nativeRecreateGfxState and then 2198x2304 once
    // the VRDevice has settled, with fresh GL texture names each time. Caching
    // one texture per stage looks right and silently gives the guest storage
    // smaller than it thinks it has (kl_glfb refuses that now, so this is what
    // makes it work rather than merely be caught). The old texture is released
    // by ARC on overwrite; kl_glfb releases the EGLImage that referenced it.
    if (!g_held[0][stage] || g_dim[stage][0] != w || g_dim[stage][1] != h ||
        g_fmt[stage] != internal_fmt) {
        if (g_held[0][stage])
            fprintf(stderr, "  [mtl] stage %d: eye changed %dx%d fmt 0x%x -> %dx%d "
                            "fmt 0x%x, reallocating\n", stage, g_dim[stage][0],
                    g_dim[stage][1], g_fmt[stage], w, h, internal_fmt);
        // One 2-slice array texture per stage, shared between the eyes — the
        // shape a layered cp_drawable has, so the slice plumbing is exercised
        // here rather than discovered on device.
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = MTLTextureType2DArray;
        d.pixelFormat = pf;                          // the guest's own format
        d.width = (NSUInteger)w; d.height = (NSUInteger)h; d.arrayLength = 2;
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;        // so the readback below works
        id<MTLTexture> t = [dev newTextureWithDescriptor:d];
        if (!t) {
            fprintf(stderr, "  [mtl] newTextureWithDescriptor 0x%x %dx%d x2 FAILED\n",
                    internal_fmt, w, h);
            return 0;
        }
        g_held[0][stage] = t; g_held[1][stage] = t;
        g_slice[0][stage] = 0; g_slice[1][stage] = 1;
        g_dim[stage][0] = w;  g_dim[stage][1] = h;
        g_fmt[stage] = internal_fmt;
        fprintf(stderr, "  [mtl] stage %d: GL 0x%x %dx%d array, 2 slices (%.1f MB)\n",
                stage, internal_fmt, w, h, (double)w * h * 8 * 2 / (1024 * 1024));
        // KL_VRR: the eye size is only known here, so the map is built here too.
        klmtl_update_rate_map(dev, w, h);
    }
    out->texture = (__bridge void *)g_held[eye][stage];
    out->slice   = g_slice[eye][stage];
    out->w       = g_dim[stage][0];
    out->h       = g_dim[stage][1];
    return 1;
}


void kl_mtl_provider_install(void) {
    // KL_VIEW implies it: the viewer's hardware compositor samples exactly
    // these textures, so without a provider there is nothing to composite and
    // the viewer would silently fall back to the readback path it exists to
    // avoid. KL_VIEW_CPU=1 is how you ask for that fallback on purpose.
    if (!getenv("KL_GLFB_MTL") &&
        !(getenv("KL_VIEW") && !getenv("KL_VIEW_CPU"))) return;
    if (!kl_glfb_enabled()) {
        fprintf(stderr, "  [mtl] KL_GLFB_MTL needs KL_GLFB=1 — ignoring\n");
        return;
    }
    kl_glfb_set_mtl_provider(provide, NULL);
    fprintf(stderr, "  [mtl] eye textures will be MTLTexture-backed\n");
}

// Read (eye, stage) back through Metal and count lit pixels, on a stride so a
// 2198x2304 RGBA16F slice is not 20 MB of work per readback.
//
// "Lit" has to mean EXACTLY what kl_glfb's capture means by it or the comparison
// is worthless, and the first version of this got it wrong in a way that looked
// like a pipeline failure: the capture tone-maps each channel to 8-bit with
// gamma 1/2.2 and then thresholds the *sum of three channels* at > 12, which is
// far more permissive than it sounds — about 0.0001 in linear terms. Thresholding
// linear luma at 12/255 instead is ~360x stricter, so a correctly-carried frame
// read as 6% lit against the reference's 100% and the obvious conclusion was the
// wrong one. Same tone map, same threshold, or no number at all.
static uint8_t tone8(float c) {
    if (!(c > 0)) return 0;                        // also NaN
    if (c > 1) c = 1;
    return (uint8_t)(powf(c, 1.0f / 2.2f) * 255.0f + 0.5f);
}

// ...and the format has to be the TEXTURE's, for the same reason. The provider
// allocates whatever internalformat the guest asked for, so a readback that
// assumes RGBA16F is wrong for any other: a 1.40 eye is
// R8G8B8A8_sRGB, so eight bytes were read where four exist: the picture landed
// in the left third of the image, magenta, with the rest of the row read out of
// the next one — indistinguishable from a broken interop, and it WAS the
// instrument. An instrument that cannot say which format it is looking at can
// only agree with itself.
//
// The rule matches kl_glfb's capture exactly, which is the whole point of these
// numbers: a float target is tone-mapped to 8 bits (klfb_dbg_tone), an 8-bit
// target is read as-is, and "lit" is the sum of three channels above 12.
static int klmtl_read_format(MTLPixelFormat pf, size_t *bpp, int *is_half) {
    switch (pf) {
    case MTLPixelFormatRGBA16Float:     *bpp = 8; *is_half = 1; return 1;
    case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Unorm:      *bpp = 4; *is_half = 0; return 1;
    default:                            return 0;
    }
}

// One channel of pixel x, in the capture's 8-bit domain.
static uint8_t klmtl_chan8(const void *row, size_t x, int k, int is_half) {
    if (!is_half) return ((const uint8_t *)row)[x * 4 + k];
    uint16_t hb = ((const uint16_t *)row)[x * 4 + k];
    int sgn = (hb >> 15) & 1, ex = (hb >> 10) & 0x1f, mant = hb & 0x3ff;
    float v;
    if (ex == 0)       v = (float)mant * (1.f / 16777216.f);
    else if (ex == 31) v = 65504.f;
    else {
        union { uint32_t u; float f; } u;
        u.u = (uint32_t)((ex - 15 + 127) << 23) | (uint32_t)(mant << 13);
        v = u.f;
    }
    return tone8(sgn ? -v : v);
}

// A texture this readback can call `getBytes` on.
//
// The GL path's eye textures are MTLStorageModeShared because the host
// allocated them that way for exactly this. The VULKAN path's are not: MoltenVK
// backs the guest's eye VkImage with a PRIVATE texture, because kl_vulkan.c asks
// for DEVICE_LOCAL memory — which is the right thing for storage the GPU renders
// into every frame, and unreadable from the CPU.
//
// So a private texture is staged through a shared one with a blit, rather than
// refused. Refusing it was worse than useless here: `kl_mtl_count_lit` returned
// 0, and 0 lit is printed as `<<< BLACK` — a diagnostic that reports a working
// pipeline as a broken one, which is the failure this whole file exists to
// avoid making.
//
// The staged copy is one SLICE, so the returned texture is always a plain 2D
// one and the caller's slice becomes 0.
static id<MTLTexture> klmtl_readable(id<MTLTexture> t, NSUInteger slice,
                                     NSUInteger *out_slice) {
    if (out_slice) *out_slice = slice;
    if (!t || t.storageMode == MTLStorageModeShared) return t;

    id<MTLDevice> dev = t.device;
    MTLTextureDescriptor *d = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:t.pixelFormat
                                     width:t.width
                                    height:t.height
                                 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> dst = [dev newTextureWithDescriptor:d];
    if (!dst) return nil;

    // A queue per call. This runs once at the end of a run, so the allocation is
    // free and it keeps the readback from owning any state.
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t
              sourceSlice:slice
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(t.width, t.height, 1)
                toTexture:dst
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (out_slice) *out_slice = 0;
    return dst;
}

static unsigned long long g_mtl_lum_sum;
static unsigned long long g_mtl_lum_n;

unsigned kl_mtl_mean_luma(void) {
    return g_mtl_lum_n ? (unsigned)(g_mtl_lum_sum / g_mtl_lum_n) : 0;
}

unsigned long kl_mtl_count_lit(int eye, int stage, int *out_w, int *out_h) {
    g_mtl_lum_sum = g_mtl_lum_n = 0;
    int slice = 0;
    void *texp = kl_glfb_eye_mtl_texture(eye, stage, &slice);
    if (!texp) return 0;
    id<MTLTexture> t = (__bridge id<MTLTexture>)texp;
    NSUInteger w = t.width, h = t.height;
    if (out_w) *out_w = (int)w;
    if (out_h) *out_h = (int)h;
    NSUInteger rslice = 0;
    t = klmtl_readable(t, (NSUInteger)slice, &rslice);
    if (!t) {
        fprintf(stderr, "  [mtl] eye texture could not be staged for readback\n");
        return 0;
    }
    slice = (int)rslice;
    size_t bpp = 0; int is_half = 0;
    if (!klmtl_read_format(t.pixelFormat, &bpp, &is_half)) {
        fprintf(stderr, "  [mtl] eye texture is MTLPixelFormat %u, which this "
                        "readback cannot decode — no count rather than a wrong "
                        "one\n", (unsigned)t.pixelFormat);
        return 0;
    }
    const NSUInteger step = 8;                 // every 8th row and column
    size_t row_bytes = w * bpp;
    void *row = malloc(row_bytes);
    if (!row) return 0;
    unsigned long lit = 0;
    for (NSUInteger y = 0; y < h; y += step) {
        [t getBytes:row bytesPerRow:row_bytes bytesPerImage:row_bytes
         fromRegion:MTLRegionMake2D(0, y, w, 1) mipmapLevel:0 slice:(NSUInteger)slice];
        for (NSUInteger x = 0; x < w; x += step) {
            unsigned lum = 0;
            for (int k = 0; k < 3; k++)
                lum += klmtl_chan8(row, x, k, is_half);
            g_mtl_lum_sum += lum;
            g_mtl_lum_n++;
            if (lum > 12) lit++;                    // kl_glfb's own threshold
        }
    }
    free(row);
    return lit;
}

// The eye's MTLTexture as a PNG, tone-mapped exactly as kl_glfb's capture is, so
// the two files can be looked at side by side. This is what a lit count cannot
// answer: not "did pixels arrive" but "are they the same picture".
// Full resolution, no stride — it runs once at the end of a run.
int kl_mtl_dump_png(int eye, int stage, const char *path) {
    int slice = 0;
    void *texp = kl_glfb_eye_mtl_texture(eye, stage, &slice);
    if (!texp || !path) return 0;
    id<MTLTexture> t = (__bridge id<MTLTexture>)texp;
    int top_left = kl_glfb_eye_mtl_origin_top_left(eye, stage);
    NSUInteger rslice = 0;
    t = klmtl_readable(t, (NSUInteger)slice, &rslice);
    if (!t) return 0;
    slice = (int)rslice;
    int32_t w = (int32_t)t.width, h = (int32_t)t.height;
    size_t bpp = 0; int is_half = 0;
    if (!klmtl_read_format(t.pixelFormat, &bpp, &is_half)) return 0;
    size_t row_bytes = (size_t)w * bpp;
    void *row = malloc(row_bytes);
    size_t stride = (size_t)w * 4, raw_n = (stride + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    if (!row || !raw) { free(row); free(raw); return 0; }
    for (int32_t y = 0; y < h; y++) {
        [t getBytes:row bytesPerRow:row_bytes bytesPerImage:row_bytes
         fromRegion:MTLRegionMake2D(0, (NSUInteger)y, (NSUInteger)w, 1)
        mipmapLevel:0 slice:(NSUInteger)slice];
        // Flipped, because GL rendered into this texture with its origin at the
        // BOTTOM left while Metal (and PNG) put row 0 at the top. Without the
        // flip the picture is upside down — which is how this was found, and is
        // exactly the mistake the compositor pass must not make when it samples
        // these textures for real.
        //
        // ...and NOT flipped when a Vulkan guest rendered it, because Vulkan's
        // origin is the top left like Metal's. The flip is a property of the API
        // the guest drew with, so it is asked rather than assumed — the
        // unconditional version put BONELAB's loading screen upside down with
        // every other part of the seam working.
        uint8_t *o = raw + (stride + 1) *
                     (size_t)(top_left ? y : (h - 1 - y));
        *o++ = 0;                                  // PNG filter: none
        for (int32_t x = 0; x < w; x++) {
            for (int k = 0; k < 3; k++)
                o[x * 4 + k] = klmtl_chan8(row, (size_t)x, k, is_half);
            o[x * 4 + 3] = 255;
        }
    }
    free(row);
    uLongf cn = compressBound((uLong)raw_n);
    uint8_t *cb = malloc(cn);
    if (!cb || compress2(cb, &cn, raw, (uLong)raw_n, 6) != Z_OK) {
        free(raw); free(cb); return 0;
    }
    free(raw);
    FILE *f = fopen(path, "wb");
    if (!f) { free(cb); return 0; }
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    // Chunk helper, inline: length, type, payload, CRC — all big-endian.
    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    struct { const char *ty; const uint8_t *d; size_t n; } chunks[] = {
        { "IHDR", ihdr, sizeof ihdr }, { "IDAT", cb, (size_t)cn }, { "IEND", NULL, 0 },
    };
    for (int i = 0; i < 3; i++) {
        uint32_t n = (uint32_t)chunks[i].n;
        uint8_t be[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
        fwrite(be, 1, 4, f);
        uLong crc = crc32(0, (const Bytef *)chunks[i].ty, 4);
        fwrite(chunks[i].ty, 1, 4, f);
        if (chunks[i].n) {
            crc = crc32(crc, chunks[i].d, (uInt)chunks[i].n);
            fwrite(chunks[i].d, 1, chunks[i].n, f);
        }
        uint8_t cbe[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16),
                           (uint8_t)(crc >> 8), (uint8_t)crc };
        fwrite(cbe, 1, 4, f);
    }
    fclose(f);
    free(cb);
    return 1;
}
