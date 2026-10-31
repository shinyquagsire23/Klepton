// libmediandk.so — AMediaCodec, AMediaFormat, AImageReader, AImage — and the
// AHardwareBuffer that ties them to the graphics side.
//
// This is the guest-facing ABI. The decoder itself is kl_vtdec.c; the split is
// the same one kl_opensl.c/kl_audio.c already make, and it is what lets `make
// hevc` check the decode with no guest at all.
//
// What the guest does with this surface, measured from its own error strings
// (QSVLCodecNDK::Init):
//
//   AMediaCodec_createDecoderByType("video/hevc")
//   AMediaFormat_new + setString(mime) + setInt32(width/height/colour/...)
//   AImageReader_newWithUsage(1, 1, AIMAGE_FORMAT_PRIVATE, GPU_SAMPLED_IMAGE|..., n)
//   AImageReader_setImageListener + AImageReader_getWindow
//   AMediaCodec_configure(codec, format, thatWindow, NULL, 0) + _start
//
// then, per frame, the ordinary MediaCodec dance: dequeue an input buffer, fill
// it, queue it; dequeue an output buffer, release it with render=true; the
// listener fires; acquireLatestImage -> AImage_getHardwareBuffer -> the
// renderer wraps that in an EGLImage (kl_egl.c).
//
// AIMAGE_FORMAT_PRIVATE is what makes this tractable: it is the guest promising
// never to touch the pixels on the CPU. The only consumer of our
// "AHardwareBuffer" is our own eglCreateImageKHR, so the handle can be whatever
// suits both ends — and it is a CVPixelBuffer, which VideoToolbox produces and
// Metal can wrap without a copy.
//
// The 1x1 in newWithUsage is not a typo either: with a producer surface, the
// reader takes its size from the producer, so the real dimensions arrive from
// the decoder rather than from that call.
#ifndef KL_MEDIANDK_H
#define KL_MEDIANDK_H

#include <stdio.h>

void *kl_mediandk_dlopen(const char *soname);   // NULL if not one of ours
int   kl_mediandk_claims(const char *soname);   // the same test, without opening
int   kl_mediandk_is_handle(const void *h);
void *kl_mediandk_sym(const char *name);        // never NULL — dlsym semantics

// ...and the ELF-import door, which must be able to say no: a name we do not
// serve has to stay in the unresolved-import report rather than becoming a
// silent stub. Same discipline as kl_openxr_lookup. NULL if unknown.
void *kl_mediandk_lookup(const char *name);

// The graphics side asks this one question of us: given the AHardwareBuffer the
// guest pulled out of an AImage, what CVPixelBuffer is it? Returns NULL if the
// handle is not one of ours, which is how kl_egl.c tells a video buffer from
// whatever else a guest might hand eglCreateImageKHR.
//
// Declared void* rather than CVPixelBufferRef so that callers who only pass it
// along do not have to include CoreVideo.
void *kl_mediandk_buffer_pixels(const void *ahardwarebuffer);

// The frontend's stored "try video anyway" choice: 1 turns the container
// demuxer on even for a guest where it defaults off (a Vulkan one — see
// klm_extractor_enabled), anything else leaves the default alone. Before boot.
void kl_mediandk_set_video_default(int on);

// SurfaceTexture uses the same bounded decoded-frame queue as AImageReader.
void *kl_mediandk_texture_new(unsigned texture);
void *kl_mediandk_texture_window(void *reader);
void kl_mediandk_texture_listener(void *reader, void *context, void (*callback)(void *, void *));
int kl_mediandk_texture_update(void *reader);
long long kl_mediandk_texture_timestamp(void *reader);
void kl_mediandk_texture_size(void *reader, int width, int height);
void kl_mediandk_texture_attach(void *reader, unsigned texture);
void kl_mediandk_texture_detach(void *reader);
void kl_mediandk_texture_delete(void *reader);
int kl_mediandk_texture_selftest(FILE *f);
void kl_mediandk_report(FILE *f);

#endif
