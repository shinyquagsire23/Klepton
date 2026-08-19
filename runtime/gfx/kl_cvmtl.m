#include "kl_cvmtl.h"
#include "kl_glfb.h"

#include <pthread.h>
#include <stdio.h>

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

// One cache for the process. A CVMetalTextureCache is a per-device object and
// there is exactly one device here (ANGLE's); per-frame creation would defeat
// the cache's whole purpose, which is reusing the texture wrappers for a
// decoder pool's recycled buffers.
static CVMetalTextureCacheRef g_cache;
static pthread_mutex_t g_cache_lk = PTHREAD_MUTEX_INITIALIZER;

static CVMetalTextureCacheRef cache(void) {
    pthread_mutex_lock(&g_cache_lk);
    if (!g_cache) {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)kl_glfb_mtl_device();
        if (!dev) {
            pthread_mutex_unlock(&g_cache_lk);
            fprintf(stderr, "  [cvmtl] no ANGLE MTLDevice — is KL_GLFB=1 and the "
                            "Metal backend up?\n");
            return NULL;
        }
        CVReturn r = CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, dev, NULL,
                                               &g_cache);
        if (r != kCVReturnSuccess) {
            pthread_mutex_unlock(&g_cache_lk);
            fprintf(stderr, "  [cvmtl] CVMetalTextureCacheCreate failed: %d\n", (int)r);
            return NULL;
        }
    }
    CVMetalTextureCacheRef c = g_cache;
    pthread_mutex_unlock(&g_cache_lk);
    return c;
}

void *kl_cvmtl_texture(void *pixel_buffer, unsigned long mtl_pixel_format,
                       void **out_cvtex) {
    if (out_cvtex) *out_cvtex = NULL;
    if (!pixel_buffer || !out_cvtex) return NULL;
    CVMetalTextureCacheRef c = cache();
    if (!c) return NULL;
    CVPixelBufferRef pb = (CVPixelBufferRef)pixel_buffer;
    // Plane 0's dimensions, which for the single-plane YCbCr formats are the
    // full picture's — the chroma plane is reached through the format, not
    // through a second texture.
    size_t w = CVPixelBufferGetWidthOfPlane(pb, 0);
    size_t h = CVPixelBufferGetHeightOfPlane(pb, 0);
    CVMetalTextureRef tex = NULL;
    CVReturn r = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, c, pb, NULL, (MTLPixelFormat)mtl_pixel_format,
        w, h, 0, &tex);
    if (r != kCVReturnSuccess || !tex) {
        static unsigned said;
        if (said++ < 4)
            fprintf(stderr, "  [cvmtl] CVMetalTextureCacheCreateTextureFromImage "
                            "failed for %zux%zu format %lu: %d\n",
                    w, h, mtl_pixel_format, (int)r);
        return NULL;
    }
    id<MTLTexture> mtl = CVMetalTextureGetTexture(tex);
    if (!mtl) {
        CFRelease(tex);
        fprintf(stderr, "  [cvmtl] CVMetalTextureGetTexture returned nothing\n");
        return NULL;
    }
    *out_cvtex = (void *)tex;
    return (__bridge void *)mtl;
}

void kl_cvmtl_release(void *cvtex) {
    if (cvtex) CFRelease((CVMetalTextureRef)cvtex);
}
