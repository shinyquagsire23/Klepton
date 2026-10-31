#ifndef KL_UNICODE_H
#define KL_UNICODE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Flat C bridge only: no Android/Apple C++ objects cross this boundary.
// Output is a top-down premultiplied RGBA8 bitmap, like Android ARGB_8888 pixels.
int kl_unicode_draw_glyph(uint32_t scalar,int style,int underline,int strike,
                          uint8_t *rgba,uint32_t width,uint32_t height,
                          float size,float baseline);
#ifdef __cplusplus
}
#endif
#endif
