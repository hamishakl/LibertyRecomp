#include "renderer_state.h"
#include "gpu_pass_timer.h"
#include "pass_contracts.h"

#include <bit>

namespace rex::graphics::gta4_metal {

std::shared_ptr<TextureResource> Renderer::State::PrepareTexture(uint32_t handle,
    const xenos::xe_gpu_texture_fetch_t& fetch, std::string& error) {
  const auto* record = resources.Virtual(handle);
  if (!record || !record->packed_depth_source)
    return resources.Texture(handle, fetch, commands, [&] { EndRender(); }, error);
  const uint32_t source_handle = record->packed_depth_source;
  if (source_handle == handle || record->guest_write_conflict) {
    error = "Packed depth alias has an invalid source or conflicting guest write"; return {};
  }
  auto source = resources.FindTexture(source_handle);
  if (!source || !source->gpu_produced || source->packed_alias ||
      !source->subresource_writes.contains(0) || source->image.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 ||
      source->image.textureType != MTLTextureType2D || source->image.mipmapLevelCount != 1) {
    error = "Packed depth alias requires a produced, resolved depth/stencil snapshot"; return {};
  }
  gta4_native::ResolveCommand allocation{};
  allocation.destination_texture = handle;
  std::memcpy(allocation.destination_fetch, &fetch, sizeof(fetch));
  auto destination = resources.ResolveTarget(allocation, error);
  if (!destination) return {};
  if (destination->image.pixelFormat != MTLPixelFormatRGBA8Unorm ||
      destination->image.width != source->image.width || destination->image.height != source->image.height ||
      destination->image.textureType != MTLTextureType2D || destination->image.mipmapLevelCount != 1) {
    error = "Packed depth alias dimensions or output format do not match its snapshot"; return {};
  }
  if (destination->packed_alias && destination->packed_source_generation == source->generation &&
      destination->packed_source_serial == source->content_serial && destination->packed_swizzle == fetch.swizzle)
    return destination;
  ProfileRead(source,3);
  auto stencil = [source->image newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
  auto pipeline = Utility("liberty_packed_depth_alias_ps", destination->image.pixelFormat, MTLPixelFormatInvalid, 1, error);
  if (!stencil || !pipeline || !Begin(error)) {
    if (error.empty()) error = "Packed depth alias pipeline or stencil view is unavailable"; return {};
  }
  EndRender();
  ResolveConstants constants{};
  constants.mode = source->info.format == xenos::TextureFormat::k_24_8_FLOAT;
  constants.flags = fetch.swizzle;
  auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = destination->image;
  pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  gpu_pass_timer::Tag(pass,"depth-pass");
  auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
  if (!encoder) { error = "Packed depth alias encoder creation failed"; return {}; }
  encoder.label = @"Liberty packed depth snapshot";
  [encoder setRenderPipelineState:pipeline];
  [encoder setViewport:MTLViewport{0, 0, double(destination->image.width), double(destination->image.height), 0, 1}];
  [encoder setFragmentTexture:source->image atIndex:0];
  [encoder setFragmentTexture:stencil atIndex:1];
  [encoder setFragmentSamplerState:fallback_sampler atIndex:0];
  [encoder setFragmentSamplerState:fallback_sampler atIndex:1];
  [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  destination->initialized = destination->packed_alias = true;
  destination->packed_source_generation = source->generation;
  destination->packed_source_serial = source->content_serial;
  destination->packed_swizzle = fetch.swizzle;
  destination->content_serial = ++content_serial;
  destination->subresource_writes[0] = destination->content_serial;
  resources.PublishWrite(handle);
  return destination;
}

bool Renderer::State::Handoff(const gta4_native::DepthSurfaceHandoffCommand& handoff, std::string& error) {
  using namespace gta4_native;
  if (!IsValidForwardStencilHandoffPolicy(handoff.stencil_policy) || !handoff.source_texture ||
      !handoff.destination.handle || handoff.source.handle == handoff.destination.handle) {
    error = "Invalid explicit depth handoff contract"; return false;
  }
  if (!MaterializePendingClears(error)) return false;
  auto source = resources.FindTexture(handoff.source_texture);
  auto destination = resources.FindSurface(handoff.destination.handle);
  if (!destination) destination = resources.Surface(handoff.destination, true, error);
  if (!source || !destination || !destination->depth || !source->subresource_writes.contains(0) ||
      source->image.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 ||
      source->image.textureType != MTLTextureType2D || source->image.sampleCount != 1 ||
      source->image.width != destination->image.width || source->image.height != destination->image.height) {
    if (error.empty()) error = "Depth handoff requires a matching resolved snapshot and destination"; return false;
  }
  ProfileRead(source,4);
  const bool rebuild = handoff.stencil_policy == ForwardStencilHandoffPolicy::kRebuildSceneCoverage;
  const char* name = rebuild ? "liberty_scene_depth_handoff_ps" : "liberty_depth_handoff_ps";
  auto pipeline = Utility(name, MTLPixelFormatInvalid, destination->image.pixelFormat,
      uint32_t(destination->image.sampleCount), error);
  FixedState fixed{}; fixed.depth_enable = fixed.depth_write_enable = 1; fixed.depth_function = 7;
  fixed.stencil_enable = rebuild; fixed.stencil_function = 7; fixed.stencil_pass = 2;
  fixed.stencil_mask = fixed.stencil_write_mask = rebuild ? 255 : 0;
  auto depth_state = DepthState(fixed, true, error);
  if (!pipeline || !depth_state || !Begin(error)) return false;
  EndRender();
  auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.depthAttachment.texture = pass.stencilAttachment.texture = destination->image;
  pass.depthAttachment.loadAction = rebuild ? MTLLoadActionClear : MTLLoadActionDontCare;
  pass.depthAttachment.clearDepth = 0;
  pass.stencilAttachment.loadAction = rebuild || !destination->initialized ? MTLLoadActionClear : MTLLoadActionLoad;
  pass.stencilAttachment.clearStencil = rebuild ? kForwardEmptySceneStencil : 0;
  pass.depthAttachment.storeAction = pass.stencilAttachment.storeAction = MTLStoreActionStore;
  gpu_pass_timer::Tag(pass,"depth-pass");
  auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
  if (!encoder) { error = "Explicit depth handoff encoder creation failed"; return false; }
  encoder.label = rebuild ? @"Liberty scene coverage rebuild" : @"Liberty depth handoff preserving stencil";
  [encoder setRenderPipelineState:pipeline]; [encoder setDepthStencilState:depth_state];
  [encoder setStencilReferenceValue:kForwardCoveredSceneStencil];
  [encoder setViewport:MTLViewport{0, 0, double(destination->image.width), double(destination->image.height), 0, 1}];
  [encoder setFragmentTexture:source->image atIndex:0];
  [encoder setFragmentSamplerState:fallback_sampler atIndex:0];
  const std::array<uint32_t, 4> constants{uint32_t(source->info.format == xenos::TextureFormat::k_24_8_FLOAT), 0, 0, 0};
  [encoder setFragmentBytes:constants.data() length:sizeof(constants) atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  destination->initialized = true;
  destination->content_mask |= kContentDepth |
      (rebuild || pass.stencilAttachment.loadAction == MTLLoadActionClear ? kContentStencil : 0);
  destination->content_serial = ++content_serial;
  resources.PublishWrite(handoff.destination.handle);
  return true;
}
}  // namespace rex::graphics::gta4_metal
