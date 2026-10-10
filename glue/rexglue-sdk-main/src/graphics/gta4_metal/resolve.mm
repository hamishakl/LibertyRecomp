#include <rex/cvar.h>
#include "renderer_state.h"
#include "gpu_pass_timer.h"
#include "../gta4_native/native_msaa_policy.h"
#include "pass_contracts.h"
#include <rex/graphics/gta4_native/surface_view.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <fmt/format.h>
#include <rex/logging/macros.h>

REXCVAR_DEFINE_BOOL(gta4_metal_defer_resolves, true, "GPU",
                   "Record resolves and run them on first use; drop ones that are overwritten unread");

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
    if (reflection && !SettlePendingResolves(destination->image, true, error)) return false;
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
  const bool initialize_in_pass = MergeResolveInitialization(direct, full, existing);
  // Pending work on the destination image, and pending writes into the source image, must land
  // before this resolve is recorded or exchanged. A full rewrite of the same subresource makes an
  // unread pending resolve into it dead: drop it instead of ordering behind it.
  if (full && defer_resolves) {
    const auto superseded = [&](const PendingResolve& record) {
      if (record.target != target || record.subresource != subresource) return false;
      for (const auto& later : pending_resolves) if (later.source_image == target) return false;
      return true;
    };
    const auto dropped = std::erase_if(pending_resolves, superseded);
    frame_resolve_drops += dropped;
  }
  if (!SettlePendingResolves(target, true, error) || !SettlePendingResolves(source->image, false, error))
    return false;
  if (!defer_resolves) {
    EndRender();
    FireInspect(source->image,"resolve-source",resolve.source.handle,fire_draw);
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
  if(!exchanged) {
    PendingResolve record;
    record.source_image = source->image; record.target = target; record.destination = destination;
    record.subresource = subresource; record.source_handle = resolve.source.handle;
    record.destination_handle = resolve.destination_texture;
    record.level = level; record.slice = slice; record.target_w = target_w; record.target_h = target_h;
    record.volume = volume; record.depth = depth; record.direct = direct; record.full = full;
    record.existing = existing; record.initialize_in_pass = initialize_in_pass;
    record.src_rect = src_rect; record.dst_rect = dst_rect; record.constants = constants;
    if (defer_resolves) {
      // Bounded: the oldest record runs when the list is full.
      settle_point = std::source_location::current();
      if (pending_resolves.size() >= 64 && !ExecuteResolve(pending_resolves.front(), error)) return false;
      if (pending_resolves.size() >= 64) pending_resolves.erase(pending_resolves.begin());
      pending_resolves.push_back(std::move(record)); ++frame_resolve_deferrals;
    } else if (!ExecuteResolve(record, error)) {
      return false;
    }
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
  if (!defer_resolves) FireInspect(target,"resolve-destination",resolve.destination_texture,fire_draw);
  return ResolveClears(resolve, error);
}
bool Renderer::State::ExecuteResolve(const PendingResolve& r, std::string& error) {
  if (!Begin(error)) return false;
  EndRender();
  ++frame_resolve_executions;
  if (profile_enabled) {
    const char* file = settle_point.file_name(); if (const char* slash = std::strrchr(file, '/')) file = slash + 1;
    REXLOG_INFO("gta4-metal-profile-resolve-execute recording={} source={:08X} destination={:08X} level={} slice={} full={} direct={} at={}:{}",
        submitted + 1, r.source_handle, r.destination_handle, r.level, r.slice, r.full, r.direct, file, settle_point.line());
  }
  // Blits retain their first partial-write initialization. Conversions merge it
  // into their load action and avoid a separate store/reload of the attachment.
  if (!r.full && !r.existing && !r.initialize_in_pass) {
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (r.depth) {
      pass.depthAttachment.texture = r.target; pass.depthAttachment.level = r.level; pass.depthAttachment.slice = r.slice;
      pass.stencilAttachment.texture = r.target; pass.stencilAttachment.level = r.level; pass.stencilAttachment.slice = r.slice;
      pass.depthAttachment.loadAction = pass.stencilAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.storeAction = pass.stencilAttachment.storeAction = MTLStoreActionStore;
      pass.depthAttachment.clearDepth = 0; pass.stencilAttachment.clearStencil = 0;
    } else {
      pass.colorAttachments[0].texture = r.target; pass.colorAttachments[0].level = r.level;
      if (r.volume) pass.colorAttachments[0].depthPlane = r.slice; else pass.colorAttachments[0].slice = r.slice;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    }
    gpu_pass_timer::Tag(pass,"resolve-draw");
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) { error = "Resolve destination initialization failed"; return false; }
    [encoder endEncoding];
  }
  if (r.direct) {
    auto blit = [commands blitCommandEncoder];
    if (!blit) { error = "Resolve copy encoder creation failed"; return false; }
    blit.label = @"Liberty exact resolve copy";
    [blit copyFromTexture:r.source_image sourceSlice:0 sourceLevel:0
        sourceOrigin:MTLOriginMake(r.src_rect.x, r.src_rect.y, 0)
        sourceSize:MTLSizeMake(r.src_rect.width, r.src_rect.height, 1) toTexture:r.target
        destinationSlice:r.volume ? 0 : r.slice destinationLevel:r.level
        destinationOrigin:MTLOriginMake(r.dst_rect.x, r.dst_rect.y, r.volume ? r.slice : 0)];
    [blit endEncoding];
  } else {
    const bool multisampled = r.source_image.sampleCount > 1;
    const char* name = r.depth ? (multisampled ? "liberty_copy_depth_stencil_msaa" : "liberty_copy_depth_stencil") :
        multisampled ? "liberty_resolve_convert_msaa_ps" : "liberty_resolve_convert_ps";
    auto pipeline = Utility(name, r.depth ? MTLPixelFormatInvalid : r.target.pixelFormat,
        r.depth ? r.target.pixelFormat : MTLPixelFormatInvalid, 1, error);
    id<MTLTexture> stencil_view = r.depth ? StencilView(r.source_image) : nil;
    FixedState fixed{}; fixed.depth_enable = r.depth; fixed.depth_function = 7; fixed.depth_write_enable = r.depth;
    fixed.stencil_enable = r.depth; fixed.stencil_function = 7; fixed.stencil_pass = 2;
    fixed.stencil_mask = fixed.stencil_write_mask = 255;
    auto depth_state = DepthState(fixed, r.depth, error);
    if (!pipeline || !depth_state || (r.depth && !stencil_view)) {
      if (error.empty()) error = "Resolve stencil sampling view failed"; return false;
    }
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (r.depth) {
      pass.depthAttachment.texture = r.target; pass.stencilAttachment.texture = r.target;
      pass.depthAttachment.level = pass.stencilAttachment.level = r.level;
      pass.depthAttachment.slice = pass.stencilAttachment.slice = r.slice;
      pass.depthAttachment.loadAction = pass.stencilAttachment.loadAction = r.full ? MTLLoadActionDontCare :
          r.initialize_in_pass ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.depthAttachment.clearDepth = 0; pass.stencilAttachment.clearStencil = 0;
      pass.depthAttachment.storeAction = pass.stencilAttachment.storeAction = MTLStoreActionStore;
    } else {
      pass.colorAttachments[0].texture = r.target; pass.colorAttachments[0].level = r.level;
      if (r.volume) pass.colorAttachments[0].depthPlane = r.slice; else pass.colorAttachments[0].slice = r.slice;
      pass.colorAttachments[0].loadAction = r.full ? MTLLoadActionDontCare :
          r.initialize_in_pass ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    gpu_pass_timer::Tag(pass,"resolve-draw");
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) { error = "Resolve conversion encoder creation failed"; return false; }
    encoder.label = r.depth ? @"Liberty depth and stencil resolve" : @"Liberty color resolve";
    [encoder setRenderPipelineState:pipeline]; [encoder setDepthStencilState:depth_state];
    [encoder setViewport:MTLViewport{0, 0, double(r.target_w), double(r.target_h), 0, 1}];
    [encoder setScissorRect:MTLScissorRect{r.dst_rect.x, r.dst_rect.y, r.dst_rect.width, r.dst_rect.height}];
    [encoder setFragmentTexture:r.source_image atIndex:0];
    if (r.depth) [encoder setFragmentTexture:stencil_view atIndex:1];
    [encoder setFragmentSamplerState:fallback_sampler atIndex:0];
    [encoder setFragmentBytes:&r.constants length:sizeof(r.constants) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
  }
  return true;
}

bool Renderer::State::SettlePendingResolves(id<MTLTexture> image, bool writing, std::string& error,
                                            std::source_location caller) {
  if (pending_resolves.empty() || !image) return true;
  settle_point = caller;
  // Select every record the access depends on, then every earlier record a selected one depends
  // on: a record depends on an earlier writer of its source or target, and on an earlier reader of
  // its target. Two readers of one source are independent, so a read of one destination does not
  // drag in every other resolve from the same (often EDRAM-aliased) source image.
  std::vector<id<MTLTexture>> written, read{image};
  if (writing) written.push_back(image);
  std::vector<bool> selected(pending_resolves.size(), false);
  const auto in = [](const std::vector<id<MTLTexture>>& set, id<MTLTexture> i) {
    return std::find(set.begin(), set.end(), i) != set.end();
  };
  bool any = false;
  for (size_t n = pending_resolves.size(); n-- > 0;) {
    const auto& r = pending_resolves[n];
    if (!in(read, r.target) && !in(written, r.target) && !in(written, r.source_image)) continue;
    selected[n] = true; any = true;
    written.push_back(r.target); read.push_back(r.source_image);
  }
  if (!any) return true;
  std::vector<PendingResolve> remaining;
  remaining.reserve(pending_resolves.size());
  bool ok = true;
  for (size_t n = 0; n < pending_resolves.size(); ++n) {
    if (!selected[n]) { remaining.push_back(std::move(pending_resolves[n])); continue; }
    if (ok && !ExecuteResolve(pending_resolves[n], error)) ok = false;
  }
  pending_resolves = std::move(remaining);
  return ok;
}

bool Renderer::State::SettlePendingResolves(const TextureResource* destination, std::string& error,
                                            std::source_location caller) {
  if (pending_resolves.empty() || !destination) return true;
  // Records name the destination by resource too: after an image exchange the resource's current
  // image and a record's target can differ, and a read needs both settled.
  for (bool again = true; again;) {
    again = false;
    for (const auto& r : pending_resolves) {
      if (r.destination.get() != destination) continue;
      if (!SettlePendingResolves(r.target, false, error, caller)) return false;
      again = true; break;
    }
  }
  return SettlePendingResolves(destination->image, false, error, caller);
}

bool Renderer::State::SettleAllPendingResolves(std::string& error, std::source_location caller) {
  if (pending_resolves.empty()) return true;
  settle_point = caller;
  auto records = std::move(pending_resolves);
  pending_resolves.clear();
  for (const auto& r : records) if (!ExecuteResolve(r, error)) return false;
  return true;
}
}  // namespace rex::graphics::gta4_metal
