#include "renderer_state.h"
#include "gpu_pass_timer.h"
#include "pass_contracts.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(gta4_metal_defer_unrelated_clears, true, "GPU",
                   "Keep unrelated attachment clears queued across a resolve");

namespace rex::graphics::gta4_metal {

id<MTLRenderPipelineState> Renderer::State::Utility(const char* name, MTLPixelFormat color,
    MTLPixelFormat depth, uint32_t samples, std::string& error) {
  const std::string key = std::string(name) + ":" + std::to_string(color) + ":" +
      std::to_string(depth) + ":" + std::to_string(samples);
  if (const auto found = utility_pipelines.find(key); found != utility_pipelines.end())
    return found->second;
  auto descriptor = [MTLRenderPipelineDescriptor new];
  descriptor.label = [NSString stringWithUTF8String:name];
  descriptor.vertexFunction = [context->ui_library newFunctionWithName:@"liberty_present_vertex"];
  NSString* function_name = [NSString stringWithUTF8String:name];
  descriptor.fragmentFunction = [context->pass_library newFunctionWithName:function_name];
  if (!descriptor.fragmentFunction)
    descriptor.fragmentFunction = [native_library newFunctionWithName:function_name];
  if (!descriptor.vertexFunction || !descriptor.fragmentFunction) {
    error = "Missing native utility function: " + std::string(name); return nil;
  }
  descriptor.colorAttachments[0].pixelFormat = color;
  descriptor.depthAttachmentPixelFormat = depth;
  descriptor.stencilAttachmentPixelFormat = depth == MTLPixelFormatDepth32Float_Stencil8
      ? depth : MTLPixelFormatInvalid;
  descriptor.rasterSampleCount = samples;
  NSError* native_error = nil;
  auto pipeline = [context->device newRenderPipelineStateWithDescriptor:descriptor error:&native_error];
  if (!pipeline) {
    error = ui::metal::MetalError(native_error, "Native utility pipeline creation failed"); return nil;
  }
  // Finite operation/format/sample combinations; malformed requests cannot grow this forever.
  if (utility_pipelines.size() >= 128) utility_pipelines.erase(utility_pipelines.begin());
  utility_pipelines.emplace(key, pipeline);
  return pipeline;
}

bool Renderer::State::ClearSurface(const std::shared_ptr<SurfaceResource>& surface,
    uint32_t aspects, const gta4_native::ResolveRectangle& requested,
    const std::array<float, 4>& color, float depth, uint32_t stencil, std::string& error) {
  if (!surface || !surface->image || !aspects || (aspects & ~7u) ||
      (surface->depth ? bool(aspects & kContentColor) : bool(aspects & ~kContentColor))) {
    error = "Invalid attachment clear contract"; return false;
  }
  const auto image = surface->image;
  const auto rectangle = ScaleRectangle(requested, surface->descriptor.width,
      surface->descriptor.height, uint32_t(image.width), uint32_t(image.height));
  if (!rectangle.width || !rectangle.height) return true;
  if ((aspects & kContentDepth) && (!std::isfinite(depth) || depth < 0 || depth > 1)) {
    error = "Invalid depth clear value"; return false;
  }
  if (!Begin(error)) return false;
  EndRender();
  const bool full = rectangle.full(uint32_t(image.width), uint32_t(image.height));
  if (full && rex::cvar::Query<bool>("gta4_metal_fold_full_clears")) {
    if (!surface->pending_clear.aspects) {
      if (pending_clears.size() >= 64 && !MaterializePendingClears(error)) return false;
      pending_clears.push_back(surface);
    }
    surface->pending_clear.Merge(aspects, surface->depth, surface->initialized, color, depth, stencil);
    surface->content_mask |= surface->pending_clear.aspects;
    surface->initialized = true;
    surface->content_serial = ++content_serial;
    return true;
  }
  if (!MaterializePendingClears(error)) return false;
  id<MTLRenderPipelineState> pipeline = nil;
  id<MTLDepthStencilState> depth_state = nil;
  if (!full) {
    const char* name = !surface->depth ? "liberty_clear_color" :
        (aspects & kContentDepth) ? "liberty_clear_depth" : "liberty_clear_stencil";
    pipeline = Utility(name, surface->depth ? MTLPixelFormatInvalid : image.pixelFormat,
        surface->depth ? image.pixelFormat : MTLPixelFormatInvalid, uint32_t(image.sampleCount), error);
    FixedState state{};
    state.depth_enable = surface->depth;
    state.depth_function = 7;
    state.depth_write_enable = bool(aspects & kContentDepth);
    state.stencil_enable = bool(aspects & kContentStencil);
    state.stencil_function = 7;
    state.stencil_pass = 2;
    state.stencil_mask = state.stencil_write_mask = 255;
    depth_state = DepthState(state, surface->depth, error);
    if (!pipeline || !depth_state) return false;
  }
  auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
  if (surface->depth) {
    pass.depthAttachment.texture = image;
    pass.stencilAttachment.texture = image;
    pass.depthAttachment.loadAction = full && (aspects & kContentDepth) ? MTLLoadActionClear :
        surface->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
    pass.stencilAttachment.loadAction = full && (aspects & kContentStencil) ? MTLLoadActionClear :
        surface->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
    pass.depthAttachment.clearDepth = full && (aspects & kContentDepth) ? depth : 0;
    pass.stencilAttachment.clearStencil = full && (aspects & kContentStencil) ? stencil & 255u : 0;
    pass.depthAttachment.storeAction = pass.stencilAttachment.storeAction = MTLStoreActionStore;
  } else {
    pass.colorAttachments[0].texture = image;
    pass.colorAttachments[0].loadAction = full ? MTLLoadActionClear :
        surface->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = full ? MTLClearColorMake(color[0], color[1], color[2], color[3]) :
        MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  }
  gpu_pass_timer::Tag(pass,"attachment-pass");
  auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
  if (!encoder) { error = "Native attachment clear encoder creation failed"; return false; }
  encoder.label = full ? @"Liberty attachment load clear" : @"Liberty partial attachment clear";
  if (!full) {
    [encoder setRenderPipelineState:pipeline];
    [encoder setDepthStencilState:depth_state];
    [encoder setStencilReferenceValue:stencil & 255u];
    [encoder setViewport:MTLViewport{0, 0, double(image.width), double(image.height), 0, 1}];
    [encoder setScissorRect:MTLScissorRect{rectangle.x, rectangle.y, rectangle.width, rectangle.height}];
    if (surface->depth) [encoder setFragmentBytes:&depth length:sizeof(depth) atIndex:0];
    else [encoder setFragmentBytes:color.data() length:sizeof(color) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }
  [encoder endEncoding];
  surface->initialized = true;
  // Track writes performed by load actions as well as the explicit clear.
  // A first depth-only clear also initializes stencil (and conversely); its
  // resolved snapshot must include both actually produced aspects.
  surface->content_mask |= aspects;
  if (surface->depth) {
    if (pass.depthAttachment.loadAction == MTLLoadActionClear) surface->content_mask |= kContentDepth;
    if (pass.stencilAttachment.loadAction == MTLLoadActionClear) surface->content_mask |= kContentStencil;
  }
  surface->content_serial = ++content_serial;
  return true;
}

void Renderer::State::ConsumePendingClear(const std::shared_ptr<SurfaceResource>& surface) {
  if (!surface || !surface->pending_clear.aspects) return;
  surface->pending_clear = {};
  std::erase(pending_clears, surface);
  ++clear_load_folds;
}

bool Renderer::State::MaterializePendingClears(std::string& error, const SurfaceResource* only) {
  if (only && !rex::cvar::Query<bool>("gta4_metal_defer_unrelated_clears")) only = nullptr;
  // A resolve reads one selected producer image. Unrelated clears can still be
  // consumed by a later render pass's load action, avoiding a clear/store/load
  // round trip. Match the actual image, including another surface wrapper for
  // that image; guest address aliases are resolved to their owner by the caller.
  const auto needed = [&](const std::shared_ptr<SurfaceResource>& surface) {
    return surface->pending_clear.aspects && (!only || surface->image == only->image);
  };
  // In particular, do not end an active encoder for a cached resolve when all
  // outstanding clears belong to unrelated attachments.
  if (std::none_of(pending_clears.begin(), pending_clears.end(), needed)) return true;
  if (!Begin(error)) return false;
  EndRender();
  for (const auto& surface : pending_clears) {
    if (!needed(surface)) continue;
    const auto value = surface->pending_clear;
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (surface->depth) {
      pass.depthAttachment.texture = pass.stencilAttachment.texture = surface->image;
      pass.depthAttachment.loadAction = (value.aspects & kContentDepth) ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.stencilAttachment.loadAction = (value.aspects & kContentStencil) ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.depthAttachment.clearDepth = value.depth;
      pass.stencilAttachment.clearStencil = value.stencil;
      pass.depthAttachment.storeAction = pass.stencilAttachment.storeAction = MTLStoreActionStore;
    } else {
      auto attachment = pass.colorAttachments[0];
      attachment.texture = surface->image;
      attachment.loadAction = MTLLoadActionClear;
      attachment.storeAction = MTLStoreActionStore;
      attachment.clearColor = MTLClearColorMake(value.color[0], value.color[1], value.color[2], value.color[3]);
    }
    gpu_pass_timer::Tag(pass,"attachment-pass");
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) { error = "Pending clear materialization failed"; return false; }
    encoder.label = @"Liberty materialize pending clear";
    [encoder endEncoding];
    surface->pending_clear = {};
    ++clear_materializations;
  }
  std::erase_if(pending_clears, [](const auto& surface) { return !surface->pending_clear.aspects; });
  return true;
}

bool Renderer::State::Clear(const gta4_native::ClearCommand& clear, std::string& error) {
  diagnostic_point="clear";
  if (clear.flags & ~0x3Fu) { error = "Unknown attachment clear flags"; return false; }
  const auto guest = memory.Read(clear.device, gta4_native::kGuestDeviceSize);
  if (guest.empty() || !Begin(error)) { if (error.empty()) error = "Unmapped clear device"; return false; }
  Targets target;
  if (!CaptureTargets(guest, clear.flags & 15u, (clear.flags & 0x30u) != 0, target, error)) return false;
  std::array<float, 4> color{};
  for (size_t i = 0; i < color.size(); ++i) color[i] = std::bit_cast<float>(clear.color_bits[i]);
  const float depth = float(std::bit_cast<double>(clear.depth_bits));
  const gta4_native::ResolveRectangle rectangle{clear.left, clear.top, clear.right, clear.bottom};
  for (size_t i = 0; i < target.colors.size(); ++i) {
    if ((clear.flags & (1u << i)) && target.colors[i] &&
        !ClearSurface(target.colors[i], kContentColor, rectangle, color, depth, clear.stencil, error)) return false;
  }
  const uint32_t aspects = ((clear.flags & 0x10u) ? kContentDepth : 0) |
      ((clear.flags & 0x20u) ? kContentStencil : 0);
  if (aspects && target.depth && !ClearSurface(target.depth, aspects, rectangle, color, depth, clear.stencil, error))
    return false;
  ++clears; ++frame_clears;
  return true;
}

bool Renderer::State::CopyColor(id<MTLTexture> source, id<MTLTexture> destination, std::string& error) {
  if (!source || !destination || source.textureType != MTLTextureType2D ||
      destination.textureType != MTLTextureType2D || source.sampleCount != 1 || destination.sampleCount != 1) {
    error = "Invalid color copy textures"; return false;
  }
  if (source == destination) return true;
  if (!MaterializePendingClears(error)) return false;
  if (!Begin(error)) return false;
  EndRender();
  if (source.pixelFormat == destination.pixelFormat && source.width == destination.width && source.height == destination.height) {
    auto blit = [commands blitCommandEncoder];
    if (!blit) { error = "Color copy encoder creation failed"; return false; }
    blit.label = @"Liberty exact color copy";
    [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(source.width, source.height, 1) toTexture:destination
        destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding]; return true;
  }
  auto pipeline = Utility("liberty_resolve_convert_ps", destination.pixelFormat, MTLPixelFormatInvalid, 1, error);
  if (!pipeline) return false;
  ResolveConstants constants{};
  constants.mode = 1;
  constants.source_extent = {uint32_t(source.width), uint32_t(source.height)};
  constants.destination_extent = {uint32_t(destination.width), uint32_t(destination.height)};
  if (constants.source_extent != constants.destination_extent) constants.flags = 4;
  auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = destination;
  pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  gpu_pass_timer::Tag(pass,"attachment-pass");
  auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
  if (!encoder) { error = "Color conversion encoder creation failed"; return false; }
  encoder.label = @"Liberty color conversion";
  [encoder setRenderPipelineState:pipeline];
  [encoder setViewport:MTLViewport{0, 0, double(destination.width), double(destination.height), 0, 1}];
  [encoder setFragmentTexture:source atIndex:0];
  [encoder setFragmentSamplerState:fallback_sampler atIndex:0];
  [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding]; return true;
}
}  // namespace rex::graphics::gta4_metal
