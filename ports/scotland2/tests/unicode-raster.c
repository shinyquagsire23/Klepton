#include "kl_unicode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
static unsigned char rgba[108*108*4],regular[sizeof rgba];
int main(int argc,char **argv){
    const unsigned codepoints[]={'A',0x6f22,0x1f600};
    for(unsigned j=0;j<3;++j){
        assert(!kl_unicode_draw_glyph(codepoints[j],0,0,0,rgba,108,108,81,86.4f));
        unsigned count=0,color=0;
        for(unsigned i=0;i<108*108;++i)if(rgba[4*i+3]){++count;if(rgba[4*i]!=rgba[4*i+1]||rgba[4*i+1]!=rgba[4*i+2])++color;}
        assert(count>20&&count<108*108);if(j==2)assert(color>20);
        printf("UNICODE_RASTER_OK scalar=U+%X pixels=%u colorPixels=%u\n",codepoints[j],count,color);
    }
    assert(!kl_unicode_draw_glyph('A',0,0,0,regular,108,108,81,86.4f));
    assert(!kl_unicode_draw_glyph('A',3,1,1,rgba,108,108,81,86.4f));assert(memcmp(regular,rgba,sizeof rgba));
    assert(!kl_unicode_draw_glyph(' ',0,0,0,rgba,108,108,81,86.4f));
    for(unsigned i=0;i<sizeof rgba;++i)assert(!rgba[i]);
    assert(kl_unicode_draw_glyph(0xd800,0,0,0,rgba,108,108,81,86.4f)<0);
    assert(kl_unicode_draw_glyph(0x110000,0,0,0,rgba,108,108,81,86.4f)<0);
    if(argc==2){
        assert(!kl_unicode_draw_glyph('F',0,0,0,rgba,108,108,81,86.4f));
        CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx=CGBitmapContextCreate(rgba,108,108,8,108*4,space,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
        CGImageRef image=CGBitmapContextCreateImage(ctx);CFURLRef url=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8*)argv[1],strlen(argv[1]),false);
        CGImageDestinationRef dest=CGImageDestinationCreateWithURL(url,CFSTR("public.png"),1,NULL);CGImageDestinationAddImage(dest,image,NULL);assert(CGImageDestinationFinalize(dest));
        CFRelease(dest);CFRelease(url);CGImageRelease(image);CGContextRelease(ctx);CGColorSpaceRelease(space);
    }
    puts("UNICODE_STYLE_WHITESPACE_INVALID_OK");return 0;
}
