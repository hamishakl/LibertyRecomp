#include <rex/cvar.h>
#include "renderer_state.h"
#include "gpu_pass_timer.h"
#include "../gta4_native/native_msaa_policy.h"
#include "pass_contracts.h"
#include <rex/graphics/gta4_native/surface_view.h>

#include <algorithm>
#include <bit>
#include <fmt/format.h>

namespace rex::graphics::gta4_metal {
namespace {
id<MTLTexture> StencilView(id<MTLTexture> image) {
  return [image newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
}
}

bool Renderer::State::ResolveClears(const gta4_native::ResolveCommand& resolve, std::string& error) {
  using namespace gta4_native;
  if (!(resolve.flags & 0x300u)) return true;
  if (resolve.parameters_valid) { error = "Resolve clear packing override is unsupported"; return false; }
  std::array<float, 4> color{};
  for (size_t i = 0; i < color.size(); ++i) color[i] = std::bit_cast<float>(resolve.clear_color_bits[i]);
  const auto guest = memory.Read(resolve.device, kGuestDeviceSize);
  const auto clear = [&](SurfaceDescriptor descriptor, bool depth) {
    auto surface = resources.FindSurface(descriptor.handle);
    if (!surface) surface = resources.Surface(descriptor, depth, error);
    if (!surface) return false;
    const ResolveRectangle rectangle = resolve.source_rectangle_valid ? resolve.source_rectangle :
        ResolveRectangle{0, 0, int32_t(descriptor.width), int32_t(descriptor.height)};
    return ClearSurface(surface, depth ? kContentDepth | kContentStencil : kContentColor, rectangle,
        color, float(std::bit_cast<double>(resolve.clear_depth_bits)), resolve.clear_stencil, error);
  };
  if (resolve.flags & 0x100u) {
    const uint32_t index = resolve.flags & 7u;
    const auto descriptor = !guest.empty() && index < kRenderTargetCount
        ? memory.Surface(GuestWord(guest, 12432 + index * sizeof(uint32_t))) : resolve.source;
    if (!clear(descriptor, false)) return false;
  }
  if (resolve.flags & 0x200u) {
    if (guest.empty()) { error = "Resolve depth clear lacks a device snapshot"; return false; }
    if (!clear(memory.Surface(GuestWord(guest, 12448)), true)) return false;
  }
  return true;
}

bool Renderer::State::Resolve(const gta4_native::ResolveCommand& request, std::string& error) {
  using namespace gta4_native;
  diagnostic_point="resolve.validate";
  auto resolve = request;
  resolve.flags = NormalizeResolveSampleFlags(resolve.flags, resolve.source.sample_type);
  const bool depth = (resolve.flags & 7u) == 4;
  if ((resolve.flags & 7u) > 4 || !resolve.source.handle || !resolve.destination_texture) {
    error = "Invalid resolve resource or source attachment"; return false;
  }
  if (!Begin(error)) return false;
  diagnostic_point="resolve.source";
  auto source = resources.FindSurface(resolve.source.handle);
  if (!source) source = resources.Surface(resolve.source, depth, error);
  if (!depth) {
    if (auto owner = resources.FindColorResolveSource(resolve.source)) source = std::move(owner);
  }
  // A pending clear already has its logical write version. Select the latest
  // producer first, then materialize only the image this resolve will read.
  if (!source || !MaterializePendingClears(error, source.get())) return false;
  diagnostic_point="resolve.destination";
  auto destination = resources.ResolveTarget(resolve, error);
  if (!destination) return false;
  diagnostic_point="resolve.content-and-copy";
  if (source->depth != depth ||
      (destination->image.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) != depth) {
    error = "Resolve color/depth aspect mismatch"; return false;
  }
  const uint32_t level = resolve.destination_level, slice = resolve.destination_slice_or_face;
  auto target = destination->image;
  const bool volume = target.textureType == MTLTextureType3D;
  if (level >= target.mipmapLevelCount || level >= 32) {
    error = "Resolve destination mip is out of range"; return false;
  }
  const uint32_t slice_count = volume ? std::max(1u, uint32_t(target.depth) >> level) :
      target.textureType == MTLTextureTypeCube ? 6u : uint32_t(target.arrayLength);
  if (level >= target.mipmapLevelCount || level >= 32 || slice >= slice_count) {
    error = "Resolve destination subresource is out of range"; return false;
  }
  const uint64_t subresource = (uint64_t(level) << 32) | slice;
  const uint32_t required = depth ? kContentDepth | kContentStencil : kContentColor;
  if ((source->content_mask & required) != required) {
    const bool reflection = resources.IsReflection(resolve.source.handle) ||
        resources.IsReflection(resolve.destination_texture);
    // Reflection textures may retain an initialized fallback before their first
    // capture. Initialization is not a successful scene resolve or content owner.
    if (reflection && !resources.InitializeTextureStorage(destination, commands,
        [&] { EndRender(); }, error)) return false;
    const bool preserved = reflection && destination->storage_initialized;
    const uint32_t produced = source->content_mask;
    const bool initialized = source->initialized;
    if (!ResolveClears(resolve, error)) return false;
    if (preserved) return true;
    error = fmt::format(
        "Resolve source has no produced content: source={:08X} generation={} "
        "wanted={} produced={} initialized={} flags={:08X} destination={:08X} "
        "guest={}x{}:{} host={}x{}:{} phase={} caller={:08X}",
        resolve.source.handle, source->generation, required, produced, initialized, resolve.flags,
        resolve.destination_texture, resolve.source.width, resolve.source.height, resolve.source.sample_type,
        source->image.width, source->image.height, source->image.sampleCount, uint32_t(Phase()), resolve.trace_caller);
    error += fmt::format(" bound={:08X}/{:08X}/{:08X}/{:08X} related={}",
        color_bindings[0].handle, color_bindings[1].handle, color_bindings[2].handle,
        color_bindings[3].handle, resources.DescribeSurfaceForDiagnostics(resolve.source));
    return false;
  }
  const uint32_t logical_w = std::max(1u, (destination->info.width + 1) >> level);
  const uint32_t logical_h = std::max(1u, (destination->info.height + 1) >> level);
  const uint32_t target_w = std::max(1u, uint32_t(target.width) >> level);
  const uint32_t target_h = std::max(1u, uint32_t(target.height) >> level);
  const int32_t dx = resolve.destination_point_valid ? resolve.destination_point.x : 0;
  const int32_t dy = resolve.destination_point_valid ? resolve.destination_point.y : 0;
  if (dx < 0 || dy < 0 || uint32_t(dx) >= logical_w || uint32_t(dy) >= logical_h) {
    error = "Resolve destination origin is outside the image"; return false;
  }
  const auto& desc = resolve.source;
  auto requested = resolve.source_rectangle_valid ? resolve.source_rectangle :
      ResolveRectangle{0, 0, int32_t(desc.width), int32_t(desc.height)};
  requested.left = std::clamp(requested.left, 0, int32_t(desc.width));
  requested.top = std::clamp(requested.top, 0, int32_t(desc.height));
  requested.right = std::clamp(requested.right, requested.left, int32_t(desc.width));
  requested.bottom = std::clamp(requested.bottom, requested.top, int32_t(desc.height));
  const uint32_t copy_w = std::min(uint32_t(requested.right - requested.left), logical_w - uint32_t(dx));
  const uint32_t copy_h = std::min(uint32_t(requested.bottom - requested.top), logical_h - uint32_t(dy));
  if (!copy_w || !copy_h) { error = "Empty resolve extent"; return false; }
  requested.right = requested.left + int32_t(copy_w);
  requested.bottom = requested.top + int32_t(copy_h);
  const auto src_rect = ScaleRectangle(requested, desc.width, desc.height,
      uint32_t(source->image.width), uint32_t(source->image.height));
  const auto dst_rect = ScaleRectangle({dx, dy, dx + int32_t(copy_w), dy + int32_t(copy_h)},
      logical_w, logical_h, target_w, target_h);
  const bool full = dst_rect.full(target_w, target_h);
  const bool existing = destination->subresource_writes.contains(subresource);
  const bool scaled = src_rect.width != dst_rect.width || src_rect.height != dst_rect.height;
  const int32_t exponent = depth ? 0 : NativeResolveExponent(resolve.flags);
  const auto selection = SanitizeGuestCopySampleSelect(DecodeResolveSampleSelect(resolve.flags),
      xenos::MsaaSamples(desc.sample_type), depth);
  ResolveConstants constants{};
  constants.source_origin = {int32_t(src_rect.x), int32_t(src_rect.y)};
  constants.destination_origin = {int32_t(dst_rect.x), int32_t(dst_rect.y)};
  const auto mapping = NormalizeColorResolveSampleMapping(
      xenos::MsaaSamples(source->descriptor.sample_type), xenos::MsaaSamples(desc.sample_type),
      xenos::MsaaSamples(SampleType(uint32_t(source->image.sampleCount))), selection);
  constants.source_guest_sample_type = depth ? desc.sample_type : uint32_t(mapping.content_samples);
  constants.requested_guest_sample_type = depth ? desc.sample_type : uint32_t(mapping.requested_samples);
  constants.physical_source_sample_type = SampleType(uint32_t(source->image.sampleCount));
  constants.sample_select = depth ? uint32_t(selection) : uint32_t(mapping.sample_select);
  constants.source_extent = {src_rect.width, src_rect.height};
  constants.destination_extent = {dst_rect.width, dst_rect.height};
  constants.flags = ((uint32_t(exponent) & 63u) << 8) | (scaled ? 4u : 0u);
  if (target.pixelFormat == MTLPixelFormatRGBA16Float || target.pixelFormat == MTLPixelFormatRG16Float)
    constants.flags |= 1u;
  const bool direct = source->image.sampleCount == 1 && !scaled && !exponent &&
      constants.source_guest_sample_type == constants.requested_guest_sample_type &&
      source->image.pixelFormat == target.pixelFormat;
  ResolveReuseKey reuse{};
  reuse.recording = submitted == UINT64_MAX ? 0 : submitted + 1;
  reuse.source_image = uint64_t(reinterpret_cast<uintptr_t>((__bridge void*)source->image));
  reuse.source_generation = source->generation; reuse.source_writer = source->content_serial;
  reuse.destination_image = uint64_t(reinterpret_cast<uintptr_t>((__bridge void*)target));
  reuse.destination_generation = destination->generation;
  reuse.level = level; reuse.slice = slice; reuse.direct = direct;
  reuse.source_format = uint32_t(source->image.pixelFormat); reuse.destination_format = uint32_t(target.pixelFormat);
  reuse.conversion = std::bit_cast<std::array<uint32_t, 16>>(constants);
  reuse.image_extents = {uint32_t(source->image.width), uint32_t(source->image.height), target_w, target_h};
  if (!depth && existing && destination->last_color_resolve.Matches(reuse, destination->content_serial)) {
    // Equivalent contents do not remove the guest's clear or publication effects.
    destination->content_serial = ++content_serial;
    destination->subresource_writes[subresource] = destination->content_serial;
    destination->last_color_resolve.writer = destination->content_serial;
    resources.PublishWrite(resolve.destination_texture);
    if(profile_enabled) resolve_profile.Write(reuse,destination->content_serial,resolve.source.handle,
        resolve.destination_texture,resolve.flags&0x300u,true);
    ++resolves; ++frame_resolves; ++resolve_skips;
    return ResolveClears(resolve, error);
  }
  // A proven no-op resolve has no encoder dependency. ResolveClears still
  // materializes any required clear and ends the encoder itself when needed.
  EndRender();
  FireInspect(source->image,"resolve-source",resolve.source.handle,fire_draw);
  const bool initialize_in_pass = MergeResolveInitialization(direct, full, existing);
  // Blits retain their first partial-write initialization. Conversions merge it
  // into their load action and avoid a separate store/reload of the attachment.
  if (!full && !existing && !initialize_in_pass) {
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (depth) {
      pass.depthAttachment.texture = target; pass.depthAttachment.level = level; pass.depthAttachment.slice = slice;
      pass.stencilAttachment.texture = target; pass.stencilAttachment.level = level; pass.stencilAttachment.slice = slice;
      pass.depthAttachment.loadAction = pass.stencilAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.storeAction = pass.stencilAttachment.storeAction = MTLStoreActionStore;
      pass.depthAttachment.clearDepth = 0; pass.stencilAttachment.clearStencil = 0;
    } else {
      pass.colorAttachments[0].texture = target; pass.colorAttachments[0].level = level;
      if (volume) pass.colorAttachments[0].depthPlane = slice; else pass.colorAttachments[0].slice = slice;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    }
    gpu_pass_timer::Tag(pass,"resolve-draw");
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) { error = "Resolve destination initialization failed"; return false; }
    [encoder endEncoding];
  }
  bool exchanged=false;
  if(direct&&!depth&&full&&src_rect.full(uint32_t(source->image.width),uint32_t(source->image.height))&&
      (resolve.flags&0x100u)&&!resolve.parameters_valid&&level==0&&slice==0&&
      resolve.source.handle==source->descriptor.handle&&exchange_resolve_clear){
    const auto device_bytes=memory.Read(resolve.device,kGuestDeviceSize);
    const uint32_t slot=resolve.flags&7u;
    const uint32_t clear_handle=!device_bytes.empty()&&slot<kRenderTargetCount?GuestWord(device_bytes,12432+slot*sizeof(uint32_t)):resolve.source.handle;
    if(clear_handle==source->descriptor.handle){
      exchanged=resources.ExchangeResolveImages(source,destination);
      if(exchanged){
        target=destination->image;destination->storage_initialized=true;
        // The source is a different image version now. Do not reuse the old
        // source-to-destination identity after its mandatory clear.
        reuse.recording=0;++resolve_image_exchanges;
      }
    }
  }
  if(exchanged){
    // The exact produced image has changed ownership, not contents.
  }else if (direct) {
    auto blit = [commands blitCommandEncoder];
    if (!blit) { error = "Resolve copy encoder creation failed"; return false; }
    blit.label = @"Liberty exact resolve copy";
    [blit copyFromTexture:source->image sourceSlice:0 sourceLevel:0
        sourceOrigin:MTLOriginMake(src_rect.x, src_rect.y, 0)
        sourceSize:MTLSizeMake(src_rect.width, src_rect.height, 1) toTexture:target
        destinationSlice:volume ? 0 : slice destinationLevel:level
        destinationOrigin:MTLOriginMake(dst_rect.x, dst_rect.y, volume ? slice : 0)];
    [blit endEncoding];
  } else {
    const bool multisampled = source->image.sampleCount > 1;
    const char* name = depth ? (multisampled ? "liberty_copy_depth_stencil_msaa" : "liberty_copy_depth_stencil") :
        multisampled ? "liberty_resolve_convert_msaa_ps" : "liberty_resolve_convert_ps";
    auto pipeline = Utility(name, depth ? MTLPixelFormatInvalid : target.pixelFormat,
        depth ? target.pixelFormat : MTLPixelFormatInvalid, 1, error);
    id<MTLTexture> stencil_view = depth ? StencilView(source->image) : nil;
    FixedState fixed{}; fixed.depth_enable = depth; fixed.depth_function = 7; fixed.depth_write_enable = depth;
    fixed.stencil_enable = depth; fixed.stencil_function = 7; fixed.stencil_pass = 2;
    fixed.stencil_mask = fixed.stencil_write_mask = 255;
    auto depth_state = DepthState(fixed, depth, error);
    if (!pipeline || !depth_state || (depth && !stencil_view)) {
      if (error.empty()) error = "Resolve stencil sampling view failed"; return false;
    }
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (depth) {
      pass.depthAttachment.texture = target; pass.stencilAttachment.texture = target;
      pass.depthAttachment.level = pass.stencilAttachment.level = level;
      pass.depthAttachment.slice = pass.stencilAttachment.slice = slice;
      pass.depthAttachment.loadAction = pass.stencilAttachment.loadAction = full ? MTLLoadActionDontCare :
          initialize_in_pass ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.depthAttachment.clearDepth = 0; pass.stencilAttachment.clearStencil = 0;
      pass.depthAttachment.storeAction = pass.stencilAttachment.storeAction = MTLStoreActionStore;
    } else {
      pass.colorAttachments[0].texture = target; pass.colorAttachments[0].level = level;
      if (volume) pass.colorAttachments[0].depthPlane = slice; else pass.colorAttachments[0].slice = slice;
      pass.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare :
          initialize_in_pass ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    gpu_pass_timer::Tag(pass,"resolve-draw");
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) { error = "Resolve conversion encoder creation failed"; return false; }
    encoder.label = depth ? @"Liberty depth and stencil resolve" : @"Liberty color resolve";
    [encoder setRenderPipelineState:pipeline]; [encoder setDepthStencilState:depth_state];
    [encoder setViewport:MTLViewport{0, 0, double(target_w), double(target_h), 0, 1}];
    [encoder setScissorRect:MTLScissorRect{dst_rect.x, dst_rect.y, dst_rect.width, dst_rect.height}];
    [encoder setFragmentTexture:source->image atIndex:0];
    if (depth) [encoder setFragmentTexture:stencil_view atIndex:1];
    [encoder setFragmentSamplerState:fallback_sampler atIndex:0];
    [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
  }
  if (initialize_in_pass) ++resolve_initializations_merged;
  destination->initialized = true;
  destination->content_serial = ++content_serial;
  destination->subresource_writes[subresource] = destination->content_serial;
  if (!depth) destination->last_color_resolve = {reuse, destination->content_serial};
  resources.PublishWrite(resolve.destination_texture);
  if(profile_enabled) resolve_profile.Write(reuse,destination->content_serial,resolve.source.handle,
      resolve.destination_texture,resolve.flags&0x300u,false);
  ++resolves; ++frame_resolves;
  FireInspect(target,"resolve-destination",resolve.destination_texture,fire_draw);
  return ResolveClears(resolve, error);
}
}  // namespace rex::graphics::gta4_metal
