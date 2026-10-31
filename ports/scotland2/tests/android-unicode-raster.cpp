#include <cstdint>
#include <cstdio>
extern "C" int sc2_unicode_draw_glyph(uint32_t,int,int,int,uint8_t*,uint32_t,uint32_t,float,float);
extern "C" __attribute__((visibility("default"))) int sc2_test_unicode_raster(){
    uint8_t pixels[108*108*4];
    for(uint32_t cp:{0x6f22u,0x1f600u}){
        if(sc2_unicode_draw_glyph(cp,0,0,0,pixels,108,108,81,86.4f))return 1;
        unsigned visible=0,color=0;
        for(unsigned i=0;i<108*108;++i)if(pixels[i*4+3]){++visible;if(pixels[4*i]!=pixels[4*i+1]||pixels[4*i+1]!=pixels[4*i+2])++color;}
        if(visible<20||(cp==0x1f600&&!color))return 2;
        std::fprintf(stderr,"UNICODE_ANDROID_ABI_RASTER_OK U+%X visible=%u color=%u\n",cp,visible,color);
    }return 0;
}
