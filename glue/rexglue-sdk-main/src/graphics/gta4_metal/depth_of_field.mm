#include "depth_of_field.h"
#include "gpu_pass_timer.h"
#include "../../ui/metal/context.h"
#include <algorithm>
#include <cstring>
namespace rex::graphics::gta4_metal {
id<MTLTexture> DepthOfField::Record(const std::shared_ptr<ui::metal::MetalContext>& context,
    id<MTLCommandBuffer> commands, id<MTLTexture> scene, id<MTLTexture> half_scene,
    id<MTLTexture> depth, id<MTLTexture> mask,
    const gta4_native::SplitPostFxParameters& parameters, std::string& error) {
  using namespace gta4_native;
  error.clear();
  const bool needs_dof = !NativeDofCanBeElided(parameters.dof_projection,
      parameters.dof_distance, parameters.dof_blur);
  const auto sampled = [](id<MTLTexture> image) {
    return image && image.textureType == MTLTextureType2D && image.sampleCount == 1;
  };
  if (!context || !context->pass_library || !commands) {
    error = "DoF command context or shader library unavailable"; return nil;
  }
  if (!ValidSplitPostFxParameters(parameters)) {
    error = "invalid-dof-constants"; return nil;
  }
  if (!sampled(scene)) { error = "scene-missing-or-not-single-sample-2d"; return nil; }
  if (!sampled(depth)) { error = "depth-missing-or-not-single-sample-2d"; return nil; }
  if (!sampled(mask)) { error = "stipple-mask-missing-or-not-single-sample-2d"; return nil; }
  if (needs_dof && (!sampled(half_scene) || half_scene.width != scene.width / 2 ||
                                          half_scene.height != scene.height / 2)) {
    error = "half-scene-missing-or-incompatible"; return nil;
  }
  const NSUInteger width = scene.width, height = scene.height;
  // Two full-size and two half-size images, worst-case RGBA32F storage.
  if (width < 2 || height < 2 || width > 16384 || height > 16384 ||
      uint64_t(width) * height * 40 > 512ull * 1024 * 1024) {
    error = "DoF intermediate extent exceeds budget"; return nil;
  }
  if (!pipeline_ || format_ != scene.pixelFormat) {
    auto descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [context->ui_library newFunctionWithName:@"liberty_present_vertex"];
    descriptor.fragmentFunction = [context->pass_library newFunctionWithName:@"liberty_split_postfx_ps"];
    descriptor.colorAttachments[0].pixelFormat = scene.pixelFormat;
    descriptor.label = @"Liberty FusionFix depth of field";
    if (!descriptor.vertexFunction || !descriptor.fragmentFunction) {
      error = "DoF shaders unavailable"; return nil;
    }
    NSError* native_error = nil;
    auto pipeline = [context->device newRenderPipelineStateWithDescriptor:descriptor error:&native_error];
    if (!pipeline) { error = ui::metal::MetalError(native_error, "DoF pipeline failed"); return nil; }
    pipeline_ = pipeline; format_ = scene.pixelFormat;
  }
  if (!sampler_) {
    auto descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    descriptor.sAddressMode = descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.lodMaxClamp = 0;
    sampler_ = [context->device newSamplerStateWithDescriptor:descriptor];
    if (!sampler_) { error = "DoF sampler allocation failed"; return nil; }
  }
  if (!images_[0] || images_[0].width != width || images_[0].height != height ||
      images_[0].pixelFormat != format_) {
    std::array<id<MTLTexture>, 4> next{};
    for (size_t i = 0; i < next.size(); ++i) {
      const bool half = i == 1 || i == 2;
      auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format_
          width:half ? width / 2 : width height:half ? height / 2 : height mipmapped:NO];
      descriptor.storageMode = MTLStorageModePrivate;
      descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
      descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
      next[i] = [context->device newTextureWithDescriptor:descriptor];
      if (!next[i]) { error = "DoF image allocation failed"; return nil; }
      next[i].label = [NSString stringWithFormat:@"Liberty DoF intermediate %zu", i];
    }
    // Already submitted command buffers retain prior extents.
    images_ = next;
  }
  SplitPostFxPushConstants push{};
  push.full_extent[0] = int32_t(width); push.full_extent[1] = int32_t(height);
  push.depth_source = uint32_t(parameters.depth_source);
  std::memcpy(push.dof_projection, parameters.dof_projection.data(), sizeof(push.dof_projection));
  std::memcpy(push.dof_distance, parameters.dof_distance.data(), sizeof(push.dof_distance));
  std::memcpy(push.dof_blur, parameters.dof_blur.data(), sizeof(push.dof_blur));
  for (uint32_t pass = 0; pass < (needs_dof ? 4u : 1u); ++pass) {
    const auto source = pass == 0 ? scene : pass == 1 ? half_scene : pass == 2 ? images_[1] : images_[0];
    const auto destination = images_[pass];
    auto descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    descriptor.colorAttachments[0].texture = destination;
    descriptor.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
    gpu_pass_timer::Tag(descriptor,"depth-of-field");
    auto encoder = [commands renderCommandEncoderWithDescriptor:descriptor];
    if (!encoder) { error = "DoF render encoder failed"; return nil; }
    encoder.label = [NSString stringWithFormat:@"Liberty DoF pass %u", pass];
    push.pass_index = pass;
    push.source_extent[0] = int32_t(source.width); push.source_extent[1] = int32_t(source.height);
    push.destination_extent[0] = int32_t(destination.width); push.destination_extent[1] = int32_t(destination.height);
    [encoder setRenderPipelineState:pipeline_];
    [encoder setViewport:MTLViewport{0, 0, double(destination.width), double(destination.height), 0, 1}];
    [encoder setFragmentBytes:&push length:sizeof(push) atIndex:0];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder setFragmentTexture:pass == 3 ? images_[2] : source atIndex:1];
    [encoder setFragmentTexture:depth atIndex:2];
    [encoder setFragmentTexture:mask atIndex:3];
    for (NSUInteger slot = 0; slot < 4; ++slot) [encoder setFragmentSamplerState:sampler_ atIndex:slot];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
  }
  // The caller publishes this private result only on success; scene is never modified.
  return images_[needs_dof ? 3 : 0];
}
}
