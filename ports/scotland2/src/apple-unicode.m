// Platform text rasterization for the original UnicodeQuest glyph/atlas code.
#include "kl_unicode.h"
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int kl_unicode_draw_glyph(uint32_t scalar,int style,int underline,int strike,
                          uint8_t *rgba,uint32_t width,uint32_t height,
                          float size,float baseline) {
    if(!rgba || !width || !height || width>4096 || height>4096 ||
       scalar>0x10ffff || (scalar>=0xd800&&scalar<=0xdfff) ||
       !isfinite(size)||size<=0||size>4096||!isfinite(baseline))return -1;
    size_t stride=(size_t)width*4;memset(rgba,0,stride*height);
    UniChar chars[2];CFIndex length=1;
    if(scalar>0xffff){uint32_t n=scalar-0x10000;chars[0]=0xd800+(n>>10);chars[1]=0xdc00+(n&1023);length=2;}
    else chars[0]=(UniChar)scalar;
    CFStringRef text=CFStringCreateWithCharacters(NULL,chars,length);
    CTFontRef font=CTFontCreateUIFontForLanguage(kCTFontUIFontSystem,size,NULL);
    if(!text || !font){if(text)CFRelease(text);if(font)CFRelease(font);return -2;}
    CTFontSymbolicTraits traits=(style&1?kCTFontBoldTrait:0)|(style&2?kCTFontItalicTrait:0);
    if(traits){CTFontRef styled=CTFontCreateCopyWithSymbolicTraits(font,0,NULL,traits,traits);if(styled){CFRelease(font);font=styled;}}
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    if(!space){CFRelease(text);CFRelease(font);return -2;}
    CGContextRef ctx=CGBitmapContextCreate(rgba,width,height,8,stride,space,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
    CGColorRef white=CGColorCreateGenericRGB(1,1,1,1);
    if(!text||!font||!ctx||!white){if(text)CFRelease(text);if(font)CFRelease(font);if(ctx)CGContextRelease(ctx);if(white)CGColorRelease(white);CGColorSpaceRelease(space);return -2;}
    const void *keys[]={kCTFontAttributeName,kCTForegroundColorAttributeName};
    const void *values[]={font,white};
    CFDictionaryRef attrs=CFDictionaryCreate(NULL,keys,values,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef attributed=attrs?CFAttributedStringCreate(NULL,text,attrs):NULL;
    CTLineRef line=attributed?CTLineCreateWithAttributedString(attributed):NULL;
    if(!line){if(attributed)CFRelease(attributed);if(attrs)CFRelease(attrs);CGColorRelease(white);CGContextRelease(ctx);CGColorSpaceRelease(space);CFRelease(font);CFRelease(text);return -2;}
    // Reject a missing-character box: successful rasterization must correspond
    // to a real glyph selected by the platform's font cascade.
    CFArrayRef runs=CTLineGetGlyphRuns(line);int missing=0;
    for(CFIndex i=0;i<CFArrayGetCount(runs);++i){
        CTRunRef run=(CTRunRef)CFArrayGetValueAtIndex(runs,i);
        CFIndex count=CTRunGetGlyphCount(run);CGGlyph *glyphs=calloc((size_t)count,sizeof *glyphs);
        if(count&&!glyphs){missing=1;break;}
        CTRunGetGlyphs(run,CFRangeMake(0,count),glyphs);
        for(CFIndex j=0;j<count;++j)if(!glyphs[j])missing=1;
        free(glyphs);
    }
    if(missing){CFRelease(line);CFRelease(attributed);CFRelease(attrs);CGColorRelease(white);CGContextRelease(ctx);CGColorSpaceRelease(space);CFRelease(font);CFRelease(text);return -3;}
    // Bitmap memory uses row zero at the top; preserve Android's baseline and
    // upright glyphs while CoreText lays out with the platform font cascade.
    CGContextTranslateCTM(ctx,0,height);CGContextScaleCTM(ctx,1,-1);
    CGContextSetTextMatrix(ctx,CGAffineTransformMakeScale(1,-1));
    CGContextSetShouldAntialias(ctx,true);CGContextSetAllowsAntialiasing(ctx,true);
    CGContextSetTextPosition(ctx,0,baseline);CTLineDraw(line,ctx);
    if(underline||strike){
        double advance=CTLineGetTypographicBounds(line,NULL,NULL,NULL);
        CGContextSetStrokeColorWithColor(ctx,white);CGContextSetLineWidth(ctx,fmax(1,size/18));
        if(underline){CGContextMoveToPoint(ctx,0,baseline+size/10);CGContextAddLineToPoint(ctx,advance,baseline+size/10);}
        if(strike){CGContextMoveToPoint(ctx,0,baseline-size/3);CGContextAddLineToPoint(ctx,advance,baseline-size/3);}
        CGContextStrokePath(ctx);
    }
    CFRelease(line);CFRelease(attributed);CFRelease(attrs);CGColorRelease(white);
    CGContextRelease(ctx);CGColorSpaceRelease(space);CFRelease(font);CFRelease(text);
    return 0;
}
