// CVPixelBuffer -> MTLTexture, through a CVMetalTextureCache on ANGLE's own
// MTLDevice (kl_glfb_mtl_device() — the EGL_METAL_TEXTURE_ANGLE extension
// requires the texture be allocated on it).
//
// This is the door kl_glfb.c uses to hand a decoded video frame to the guest in
// the decoder's native biplanar YCbCr: the texture is created over PLANE 0 with
// one of Apple's private single-plane YCbCr pixel formats, whose sampler
// performs the YUV->RGB conversion. Objective-C because CVMetalTextureGetTexture
// speaks id<MTLTexture>; nothing else in here is more than plumbing.
#ifndef KL_CVMTL_H
#define KL_CVMTL_H

// Returns the id<MTLTexture> (borrowed — its lifetime is *out_cvtex's), or NULL
// with the reason printed. mtl_pixel_format is passed through verbatim, so the
// private-format numbers stay the caller's decision. *out_cvtex receives the
// CVMetalTextureRef that keeps the texture's storage alive; release it with
// kl_cvmtl_release once nothing samples the texture.
void *kl_cvmtl_texture(void *pixel_buffer, unsigned long mtl_pixel_format,
                       void **out_cvtex);
void  kl_cvmtl_release(void *cvtex);

#endif
