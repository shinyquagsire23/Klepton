#define main original_reproject_main
#include "t_reproject.m"
#undef main
int main(void) { @autoreleasepool {
 id<MTLDevice> dev=MTLCreateSystemDefaultDevice(); NSError *err=nil;
 id<MTLLibrary> lib=[dev newLibraryWithSource:[NSString stringWithUTF8String:kl_reproject_msl()] options:nil error:&err];
 MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
 pd.vertexFunction=[lib newFunctionWithName:@"kl_reproject_v"]; pd.fragmentFunction=[lib newFunctionWithName:@"kl_reproject_depth_f"];
 pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm; pd.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
 id<MTLRenderPipelineState> ps=[dev newRenderPipelineStateWithDescriptor:pd error:&err];
 if(!ps) { fprintf(stderr,"%s\n",err.description.UTF8String); return 1; }
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:16 height:16 mipmapped:NO];
 td.textureType=MTLTextureType2DArray; td.arrayLength=1; td.storageMode=MTLStorageModePrivate; td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 id<MTLTexture> dep=[dev newTextureWithDescriptor:td];
 td.textureType=MTLTextureType2D; id<MTLTexture> outDepth=[dev newTextureWithDescriptor:td];
 td.pixelFormat=MTLPixelFormatRGBA8Unorm; id<MTLTexture> outColor=[dev newTextureWithDescriptor:td];
 td.textureType=MTLTextureType2DArray; id<MTLTexture> color=[dev newTextureWithDescriptor:td];
 id<MTLCommandQueue> q=[dev newCommandQueue];
 MTLDepthStencilDescriptor *dd=[MTLDepthStencilDescriptor new];dd.depthWriteEnabled=YES;dd.depthCompareFunction=MTLCompareFunctionAlways;
 id<MTLDepthStencilState> dss=[dev newDepthStencilStateWithDescriptor:dd];
 MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];id<MTLSamplerState> samp=[dev newSamplerStateWithDescriptor:sd];
 kl_reproject_uniforms u={0};u.projection=kl_reproject_projection(1,1,1,1,.03f);u.model_view=matrix_identity_float4x4;u.tangents=(simd_float4){1,1,1,1};u.depth=500;u.visible=1;
 const float cases[][3]={{.1f,100,.5f},{100,.1f,.5f},{.1f,INFINITY,2},{INFINITY,.1f,2}};
 for(int i=0;i<4;i++) {
  float n=cases[i][0],f=cases[i][1],z=cases[i][2];
  float raw=isinf(f)?1-n/z:isinf(n)?f/z:(f-n*f/z)/(f-n);
  simd_float4 dp[2]={{n,f,0,1},{0,0,0,0}};
  id<MTLCommandBuffer> cb=[q commandBuffer];
  MTLRenderPassDescriptor *clear=[MTLRenderPassDescriptor renderPassDescriptor];clear.depthAttachment.texture=dep;clear.depthAttachment.loadAction=MTLLoadActionClear;clear.depthAttachment.storeAction=MTLStoreActionStore;clear.depthAttachment.clearDepth=raw;
  [[cb renderCommandEncoderWithDescriptor:clear] endEncoding];
  MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=outColor;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
  rp.depthAttachment.texture=outDepth;rp.depthAttachment.loadAction=MTLLoadActionClear;rp.depthAttachment.clearDepth=0;rp.depthAttachment.storeAction=MTLStoreActionStore;
  id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:rp];[enc setRenderPipelineState:ps];[enc setDepthStencilState:dss];
  [enc setVertexBytes:&u length:sizeof u atIndex:0];[enc setFragmentBytes:&u length:sizeof u atIndex:0];[enc setFragmentBytes:dp length:sizeof dp atIndex:2];
  [enc setFragmentTexture:color atIndex:0];[enc setFragmentTexture:color atIndex:1];[enc setFragmentTexture:dep atIndex:2];[enc setFragmentTexture:dep atIndex:3];[enc setFragmentSamplerState:samp atIndex:0];klr_draw(enc);[enc endEncoding];
  id<MTLBuffer> buf=[dev newBufferWithLength:16*256 options:MTLResourceStorageModeShared];id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
  [be copyFromTexture:outDepth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(16,16,1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:4096];[be endEncoding];[cb commit];[cb waitUntilCompleted];
  float got=*(float *)((char *)buf.contents+8*256+8*4);float want=.03f/z;
  printf("depth case %d: got %.8f expected %.8f\n",i,got,want);if(cb.error || fabsf(got-want)>1e-5) return 1;
 }
 return 0;
}}
