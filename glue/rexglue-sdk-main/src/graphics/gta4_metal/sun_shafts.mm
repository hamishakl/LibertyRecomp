#include "sun_shafts.h"
#include "gpu_pass_timer.h"
#include "../../ui/metal/context.h"
#include <algorithm>
#include <cstring>
namespace rex::graphics::gta4_metal {
id<MTLTexture> SunShafts::Record(const std::shared_ptr<ui::metal::MetalContext>& context,
    id<MTLCommandBuffer> commands, id<MTLTexture> scene, id<MTLTexture> depth, id<MTLBuffer> cloud_mask,
    const gta4_native::SunShaftParameters& parameters, std::string& error) {
  using namespace gta4_native;
  error.clear();
  if (!parameters.valid) return scene;
  const auto sampled = [](id<MTLTexture> image) {
    return image && image.textureType == MTLTextureType2D && image.sampleCount == 1;
  };
  if (!context || !context->pass_library || !commands || !sampled(scene) ||
      !sampled(depth) || !cloud_mask || !parameters.cloud_width || !parameters.cloud_height ||
      parameters.cloud_mask_address != cloud_mask.gpuAddress ||
      uint64_t(parameters.cloud_width) * parameters.cloud_height * sizeof(float) > cloud_mask.length) {
    error = "Sun-shaft inputs unavailable"; return nil;
  }
  const NSUInteger width = scene.width, height = scene.height;
  // One full-size and three half-size images, worst-case RGBA32F storage.
  if (width < 2 || height < 2 || width > 16384 || height > 16384 ||
      uint64_t(width) * height * 28 > 512ull * 1024 * 1024) {
    error = "Sun shafts intermediate extent exceeds budget"; return nil;
  }
  if (!pipeline_ || format_ != scene.pixelFormat) {
    auto descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [context->ui_library newFunctionWithName:@"liberty_present_vertex"];
    descriptor.fragmentFunction = [context->pass_library newFunctionWithName:@"liberty_sun_shafts_ps"];
    descriptor.colorAttachments[0].pixelFormat = scene.pixelFormat;
    descriptor.label = @"Liberty FusionFix sun shafts";
    if (!descriptor.vertexFunction || !descriptor.fragmentFunction) {
      error = "Sun shafts shaders unavailable"; return nil;
    }
    NSError* native_error = nil;
    auto pipeline = [context->device newRenderPipelineStateWithDescriptor:descriptor error:&native_error];
    if (!pipeline) { error = ui::metal::MetalError(native_error, "Sun shafts pipeline failed"); return nil; }
    pipeline_ = pipeline; format_ = scene.pixelFormat;
  }
  if (!sampler_) {
    auto descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    descriptor.sAddressMode = descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.lodMaxClamp = 0;
    sampler_ = [context->device newSamplerStateWithDescriptor:descriptor];
    if (!sampler_) { error = "Sun shafts sampler allocation failed"; return nil; }
  }
  if (!images_[0] || images_[3].width != width || images_[3].height != height ||
      images_[0].pixelFormat != format_) {
    std::array<id<MTLTexture>, 4> next{};
    for (size_t i = 0; i < next.size(); ++i) {
      const bool half = i < 3;
      auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format_
          width:half ? width / 2 : width height:half ? height / 2 : height mipmapped:NO];
      descriptor.storageMode = MTLStorageModePrivate;
      descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
      descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
      next[i] = [context->device newTextureWithDescriptor:descriptor];
      if (!next[i]) { error = "Sun shafts image allocation failed"; return nil; }
      next[i].label = [NSString stringWithFormat:@"Liberty Sun shafts intermediate %zu", i];
    }
    // Already submitted command buffers retain prior extents.
    images_ = next;
  }
  for (uint32_t pass = 0; pass < 4; ++pass) {
    const auto source = pass == 0 || pass == 3 ? scene : images_[pass - 1];
    const auto destination = images_[pass];
    auto descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    descriptor.colorAttachments[0].texture = destination;
    descriptor.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
    gpu_pass_timer::Tag(descriptor,"sun-shafts");
    auto encoder = [commands renderCommandEncoderWithDescriptor:descriptor];
    if (!encoder) { error = "Sun shafts render encoder failed"; return nil; }
    encoder.label = [NSString stringWithFormat:@"Liberty Sun shafts pass %u", pass];
    const auto push = BuildSunShaftPushConstants(parameters, uint32_t(source.width), uint32_t(source.height),
        uint32_t(destination.width), uint32_t(destination.height), pass);
    [encoder setRenderPipelineState:pipeline_];
    [encoder setViewport:MTLViewport{0, 0, double(destination.width), double(destination.height), 0, 1}];
    [encoder setFragmentBytes:&push length:sizeof(push) atIndex:0];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder setFragmentTexture:pass == 3 ? images_[2] : source atIndex:1];
    [encoder setFragmentTexture:depth atIndex:2];
    [encoder useResource:cloud_mask usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    for (NSUInteger slot = 0; slot < 3; ++slot) [encoder setFragmentSamplerState:sampler_ atIndex:slot];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
  }
  // The caller publishes this private result only on success; scene is never modified.
  return images_[3];
}
}
