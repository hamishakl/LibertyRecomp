#include "resource_usage.h"
#include "resource_state.h"
#include "guest_state.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <bit>
#include <fmt/format.h>
#include <limits>

namespace rex::graphics::gta4_metal {
ResourceStore::State::State(std::shared_ptr<ui::metal::MetalContext> c,GuestMemory m)
    : context(std::move(c)),memory(m) {
  texture_budget=std::min<size_t>(2ull*1024ull*1024ull*1024ull,context->device.recommendedMaxWorkingSetSize/2);
}
ResourceStore::ResourceStore(std::shared_ptr<ui::metal::MetalContext> context, GuestMemory memory,
                             size_t buffer_capture_budget, size_t texture_budget,
                             size_t vertex_conversion_budget)
    : state_(std::make_unique<State>(std::move(context), memory)) {
  state_->buffer_capture_budget = std::min(buffer_capture_budget, kDefaultBufferCaptureBudget);
  state_->texture_budget = std::min(texture_budget, state_->texture_budget);
  state_->vertex_conversion_budget = std::min(vertex_conversion_budget, State::kVertexConversionBudget);
}
ResourceStore::~ResourceStore()=default;

MTLPixelFormat ResourceStore::TextureFormat(xenos::TextureFormat format) {
  switch(GetBaseFormat(format)) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_DXT5A: return MTLPixelFormatR8Unorm;
    case xenos::TextureFormat::k_DXN:
    case xenos::TextureFormat::k_CTX1: return MTLPixelFormatRG8Unorm;
    case xenos::TextureFormat::k_8_8_8_8: return MTLPixelFormatRGBA8Unorm;
    case xenos::TextureFormat::k_DXT1: return MTLPixelFormatBC1_RGBA;
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT3A: return MTLPixelFormatBC2_RGBA;
    case xenos::TextureFormat::k_DXT4_5: return MTLPixelFormatBC3_RGBA;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT: return MTLPixelFormatRGBA16Float;
    case xenos::TextureFormat::k_16_16_FLOAT: return MTLPixelFormatRG16Float;
    case xenos::TextureFormat::k_32_FLOAT: return MTLPixelFormatR32Float;
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT: return MTLPixelFormatDepth32Float_Stencil8;
    default: return MTLPixelFormatInvalid;
  }
}
MTLPixelFormat ResourceStore::SurfaceFormat(uint32_t format,bool depth) {
  if(depth) return format==0x1A220197u ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatInvalid;
  switch(format) {
    case 0x18280186u: return MTLPixelFormatRGBA8Unorm;
    case 0x1A2201BFu: return MTLPixelFormatRGBA16Float;
    case 0x2DA2ABA4u: return MTLPixelFormatR32Float;
    case 0x2D22AB9Fu:
    case 0x2D20AB8Du: return MTLPixelFormatRG16Float;
    default: return MTLPixelFormatInvalid;
  }
}

bool ResourceStore::State::AdmitTexture(size_t bytes, uint32_t handle, bool surface,
                                        std::string& error) {
  // Replacing a guest name retires its previous allocation from cache ownership.
  // Encoded work keeps that immutable generation alive until GPU completion.
  size_t cached = resident_texture_bytes;
  if (surface) {
    if (const auto old = surfaces.find(handle); old != surfaces.end())
      cached -= old->second->image.allocatedSize;
  } else {
    if (const auto old = textures.find(handle); old != textures.end())
      cached -= old->second->image.allocatedSize;
  }
  auto reject = [&](const char* reason) {
    ++texture_rejections;
    error = fmt::format("Metal resource cache budget exceeded: {} requested={} cached={} budget={}",
                        reason, bytes, resident_texture_bytes, texture_budget);
    return false;
  };
  if (bytes > texture_budget) return reject("allocation exceeds budget");
  const size_t limit = texture_budget - bytes;
  if (cached <= limit) return true;

  // Preflight before removing anything: a pinned-only failure must preserve all
  // names. Only guest-backed assets (including reproducible font atlases) can be
  // reloaded. Render surfaces, resolves, virtual targets and reflections cannot.
  std::vector<uint32_t> victims;
  for (uint32_t candidate : texture_lru) {
    const auto& texture = textures.at(candidate);
    if ((!surface && candidate == handle) || texture->gpu_produced ||
        virtuals.Find(candidate) || reflections.contains(candidate)) continue;
    victims.push_back(candidate);
    cached -= texture->image.allocatedSize;
    if (cached <= limit) break;
  }
  if (cached > limit) return reject("insufficient reloadable textures");
  for (uint32_t victim : victims) {
    EraseTexture(victim);
    ++texture_evictions;
  }
  return true;
}

std::shared_ptr<SurfaceResource> ResourceStore::Surface(const gta4_native::SurfaceDescriptor& guest,
    bool depth,std::string& error,uint32_t sample_override) {
  @autoreleasepool {
    error.clear();
    uint32_t width=guest.width,height=guest.height;
    if(!guest.handle || !width || !height || guest.sample_type>2) {
      error="Invalid native surface descriptor"; return {};
    }
    if(const auto* record=state_->virtuals.Find(guest.handle)) {
      if(record->kind!=gta4_native::VirtualResourceKind::kSurface ||
          record->logical_width!=width || record->logical_height!=height) {
        error="Virtual surface descriptor mismatch"; return {};
      }
      width=record->physical_width; height=record->physical_height;
    }
    uint32_t samples=sample_override?sample_override:1u<<guest.sample_type;
    if(const auto found=state_->reflections.find(guest.handle);found!=state_->reflections.end()) {
      width=found->second.physical_width; height=found->second.physical_height;
      if(found->second.sample_count_override) samples=found->second.sample_count_override;
    }
    auto format=SurfaceFormat(guest.format,depth);
    if(!width || !height || width>16384 || height>16384 || format==MTLPixelFormatInvalid) {
      error="Unsupported native surface format, extent or sample count"; return {};
    }
    if(samples>1 && format==MTLPixelFormatR32Float && !state_->context->device.supports32BitMSAA) {
      // Apple GPU family 7 (M1, A14) cannot multisample 32-bit float formats; M2 and later can.
      // Keep the pass's sample count so sibling attachments still match, and render into RG16Float
      // instead. Resolves convert by format, so the title's R32F destination texture is unchanged;
      // the 16-bit precision applies only on devices that would otherwise fail the allocation.
      static bool warned=false;
      if(!warned) {
        warned=true;
        REXLOG_WARN("gta4-metal: this GPU cannot multisample 32-bit float surfaces; surface {:08X} "
                    "({}x{}, {} samples) renders as RG16Float", guest.handle, width, height, samples);
      }
      format=MTLPixelFormatRG16Float;
    }
    auto existing=state_->surfaces.find(guest.handle);
    if(existing!=state_->surfaces.end()) {
      auto& value=existing->second;
      gta4_native::GuestSurfaceView previous_view{}, requested_view{};
      const bool same_view = gta4_native::DecodeGuestSurfaceView(value->descriptor, depth, previous_view) &&
          gta4_native::DecodeGuestSurfaceView(guest, depth, requested_view) && previous_view == requested_view;
      if(same_view && value->depth==depth && value->image.width==width && value->image.height==height &&
          value->image.pixelFormat==format && value->image.sampleCount==samples) {
        value->descriptor=guest; return value;
      }
    }
    // A matching immutable allocation already proves support on this device.
    // New formats/views/sample counts still take the complete validation path.
    if (![state_->context->device supportsTextureSampleCount:samples]) {
      error="Unsupported native surface format, extent or sample count"; return {};
    }
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    descriptor.sampleCount=samples;
    descriptor.textureType=samples==1 ? MTLTextureType2D : MTLTextureType2DMultisample;
    descriptor.storageMode=MTLStorageModePrivate;
    descriptor.hazardTrackingMode=MTLHazardTrackingModeTracked;
    descriptor.usage=NativeTextureUsage(true,depth);
    const auto allocation=[state_->context->device heapTextureSizeAndAlignWithDescriptor:descriptor];
    if(!state_->AdmitTexture(allocation.size,guest.handle,true,error)) return {};
    auto image=[state_->context->device newTextureWithDescriptor:descriptor];
    if(!image) {error="Metal surface allocation failed"; return {};}
    image.label=[NSString stringWithFormat:@"Liberty %@ surface %08X",depth?@"depth":@"color",guest.handle];
    auto resource=std::make_shared<SurfaceResource>();
    resource->image=image; resource->descriptor=guest; resource->depth=depth;
    resource->generation=state_->next_generation++;
    state_->InstallSurface(guest.handle,resource); return resource;
  }
}

std::shared_ptr<TextureResource> ResourceStore::State::AllocateTexture(uint32_t handle,
    const xenos::xe_gpu_texture_fetch_t& fetch,bool render_target,std::string& error) {
  TextureInfo info{};
  if(!handle || !TextureInfo::Prepare(fetch,&info) || info.mip_min_level>info.mip_max_level ||
      info.mip_max_level>=xenos::kTextureMaxMips) {error="Invalid guest texture descriptor"; return {};}
  const auto format=ResourceStore::TextureFormat(info.format);
  if(format==MTLPixelFormatInvalid) {error="Unsupported native texture format"; return {};}
  const bool compressed=info.is_compressed() && GetBaseFormat(info.format)!=xenos::TextureFormat::k_CTX1 &&
      GetBaseFormat(info.format)!=xenos::TextureFormat::k_DXN && GetBaseFormat(info.format)!=xenos::TextureFormat::k_DXT5A;
  if(compressed && (!context->device.supportsBCTextureCompression || render_target)) {
    error="BC texture capability/usage mismatch"; return {};
  }
  uint32_t width=info.width+1,height=info.height+1,depth=info.depth+1;
  if(const auto* record=virtuals.Find(handle)) {
    if(record->kind!=gta4_native::VirtualResourceKind::kTexture ||
        record->logical_width!=width || record->logical_height!=height) {
      error="Virtual texture dimensions disagree with title fetch"; return {};
    }
    width=record->physical_width; height=record->physical_height;
  }
  if (auto reflection = reflections.find(handle); reflection != reflections.end()) {
    if (reflection->second.logical_width != info.width + 1 || reflection->second.logical_height != info.height + 1) {
      error = "Reflection texture logical extent disagrees with registration"; return {};
    }
    width = reflection->second.physical_width; height = reflection->second.physical_height;
  }
  if(!width || !height || width>16384 || height>16384) {error="Texture extent exceeds device contract"; return {};}
  auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
  descriptor.mipmapLevelCount=info.mip_max_level+1;
  switch(info.dimension) {
    case xenos::DataDimension::k2DOrStacked:
      if(info.is_stacked) {descriptor.textureType=MTLTextureType2DArray; descriptor.arrayLength=depth;}
      break;
    case xenos::DataDimension::kCube:
      if(width!=height) {error="Non-square cube texture"; return {};}
      descriptor.textureType=MTLTextureTypeCube; break;
    case xenos::DataDimension::k3D:
      descriptor.textureType=MTLTextureType3D; descriptor.depth=depth; break;
    default: error="Unsupported texture dimension"; return {};
  }
  descriptor.storageMode=MTLStorageModePrivate;
  descriptor.hazardTrackingMode=MTLHazardTrackingModeTracked;
  descriptor.usage=NativeTextureUsage(render_target,format==MTLPixelFormatDepth32Float_Stencil8);
  const auto allocation=[context->device heapTextureSizeAndAlignWithDescriptor:descriptor];
  if(!AdmitTexture(allocation.size,handle,false,error)) return {};
  auto image=[context->device newTextureWithDescriptor:descriptor];
  if(!image) {error="Metal texture allocation failed"; return {};}
  image.label=[NSString stringWithFormat:@"Liberty %@ texture %08X",render_target?@"resolved":@"asset",handle];
  auto resource=std::make_shared<TextureResource>();
  resource->image=image; resource->fetch=fetch; resource->info=info;
  resource->generation=next_generation++; resource->gpu_produced=render_target;
  resource->use_serial=++serial;
  return resource;
}

std::shared_ptr<TextureResource> ResourceStore::ResolveTarget(const gta4_native::ResolveCommand& command,
    std::string& error) {
  @autoreleasepool {
    error.clear();
    const auto fetch=std::bit_cast<xenos::xe_gpu_texture_fetch_t>(command.destination_fetch);
    auto found=state_->textures.find(command.destination_texture);
    if(found!=state_->textures.end() && found->second->gpu_produced &&
        gta4_native::NativeTextureImageFetchEqual(found->second->fetch,fetch)) return found->second;
    auto resource=state_->AllocateTexture(command.destination_texture,fetch,true,error);
    if(resource) {state_->InstallTexture(command.destination_texture,resource); state_->dirty.erase(command.destination_texture);}
    return resource;
  }
}
bool ResourceStore::ExchangeResolveImages(const std::shared_ptr<SurfaceResource>& source,const std::shared_ptr<TextureResource>& destination){
  if(!source||!destination||source->depth||!destination->gpu_produced)return false;
  auto a=source->image,b=destination->image;
  if(!a||!b||a==b||a.textureType!=MTLTextureType2D||b.textureType!=MTLTextureType2D||a.pixelFormat!=b.pixelFormat||
     a.width!=b.width||a.height!=b.height||a.sampleCount!=1||b.sampleCount!=1||a.mipmapLevelCount!=1||b.mipmapLevelCount!=1||
     a.arrayLength!=1||b.arrayLength!=1||a.usage!=b.usage||a.allocatedSize!=b.allocatedSize)return false;
  // Both named owners keep separate physical allocations. Previously encoded
  // GPU operations retain their original objects; only future name resolution changes.
  source->image=b;destination->image=a;
  source->generation=state_->next_generation++;destination->generation=state_->next_generation++;
  state_->InvalidateBindings();return true;
}
std::shared_ptr<TextureResource> ResourceStore::FindTexture(uint32_t handle) const {
  auto found=state_->textures.find(handle); return found==state_->textures.end()?nullptr:found->second;
}
std::shared_ptr<SurfaceResource> ResourceStore::FindSurface(uint32_t handle) const {
  auto found=state_->surfaces.find(handle); return found==state_->surfaces.end()?nullptr:found->second;
}
std::shared_ptr<SurfaceResource> ResourceStore::FindColorResolveSource(
    const gta4_native::SurfaceDescriptor& descriptor) const {
  using namespace gta4_native;
  if (IsReflection(descriptor.handle)) return FindSurface(descriptor.handle);
  GuestSurfaceView view{};
  if (!DecodeGuestSurfaceView(descriptor, false, view)) return {};
  const auto group = state_->color_views.find(GetGuestPlacementKey(view));
  if (group == state_->color_views.end()) return {};
  std::shared_ptr<SurfaceResource> latest;
  for (uint32_t handle : group->second) {
    if (IsReflection(handle)) continue;
    auto candidate = FindSurface(handle);
    if (!candidate || candidate->depth || !candidate->initialized ||
        !(candidate->content_mask & 1u) || !candidate->content_serial) continue;
    if (!latest || candidate->content_serial > latest->content_serial) latest = std::move(candidate);
  }
  return latest;
}
bool ResourceStore::IsReflection(uint32_t handle) const {return state_->reflections.contains(handle);}
bool ResourceStore::IsWaterReflection(uint32_t handle) const {
  const auto it = state_->reflections.find(handle);
  return it != state_->reflections.end() && it->second.family == gta4_native::ReflectionFamily::kWater;
}
const gta4_native::NativeVirtualResourceRecord* ResourceStore::Virtual(uint32_t handle) const {
  return state_->virtuals.Find(handle);
}
void ResourceStore::PublishWrite(uint32_t handle) {
  state_->virtuals.PublishHostWrite(handle); state_->dirty.erase(handle);
}
bool ResourceStore::RegisterVirtual(const gta4_native::RegisterVirtualResourceCommand& command) {
  using namespace gta4_native;
  if(!command.resource || !command.logical_width || !command.logical_height ||
      !command.physical_width || !command.physical_height || command.physical_width>16384 ||
      command.physical_height>16384 || !command.guest_backing_width || !command.guest_backing_height ||
      (command.kind!=VirtualResourceKind::kTexture && command.kind!=VirtualResourceKind::kSurface)) return false;
  state_->InvalidateBindings();
  if(state_->virtuals.Register(command)==VirtualResourceRegistrationResult::kReplaced) {
    state_->EraseSurface(command.resource); state_->EraseTexture(command.resource);
  }
  return true;
}
void ResourceStore::RegisterReflection(const gta4_native::RegisterReflectionTargetCommand& command) {
  using namespace gta4_native;
  const NativeReflectionTarget next{command.family, command.role, command.wrapper,
      command.surface, command.texture, command.logical_width, command.logical_height,
      command.physical_width, command.physical_height, command.sample_count_override};
  const auto surface = state_->reflections.find(next.surface);
  const auto texture = state_->reflections.find(next.texture);
  if (surface != state_->reflections.end() && SameNativeReflectionRegistration(surface->second, next) &&
      (!next.texture || (texture != state_->reflections.end() &&
                        SameNativeReflectionRegistration(texture->second, next)))) return;
  // A replaced pair must not leave a stale companion classified as a reflection.
  // GPU command buffers retain any retired physical images until completion.
  for (uint32_t handle : {next.surface, next.texture}) {
    if (const auto found = state_->reflections.find(handle); found != state_->reflections.end()) {
      const auto old = found->second;
      state_->EraseSurface(old.surface); state_->EraseTexture(old.texture);
      EraseNativeReflectionRegistration(state_->reflections, handle);
    }
  }
  state_->EraseSurface(next.surface); state_->EraseTexture(next.texture);
  RegisterNativeReflectionTarget(state_->reflections, next);
}

void ResourceStore::Invalidate(uint32_t handle) {
  state_->InvalidateBindings();
  state_->preparation.Invalidate(handle);
  state_->dirty.insert(handle);
  if(state_->virtuals.MarkGuestWrite(handle)) {
    state_->EraseTexture(handle);
    if(const auto* record=state_->virtuals.Find(handle); record && record->companion) {
      state_->virtuals.MarkGuestWrite(record->companion); state_->EraseTexture(record->companion);
      state_->EraseSurface(record->companion);
    }
  }
}
void ResourceStore::Release(uint32_t handle) {
  state_->InvalidateBindings();
  state_->EraseTexture(handle); state_->EraseSurface(handle); state_->EraseBuffer(handle);
  state_->virtuals.Erase(handle); state_->font_ids.erase(handle);
  gta4_native::EraseNativeReflectionRegistration(state_->reflections, handle);
  state_->dirty.erase(handle);
}
void ResourceStore::Clear() {
  state_->InvalidateBindings();
  state_->preparation.Clear();
  state_->vertex_conversion_lru.clear();
  state_->vertex_conversion_bytes = state_->vertex_conversion_budget_bytes = 0;
  state_->textures.clear(); state_->surfaces.clear(); state_->buffers.clear();
  state_->texture_lru.clear();
  state_->color_views.clear(); state_->buffer_lru.clear(); state_->font_ids.clear();
  state_->resident_texture_bytes = state_->captured_buffer_bytes = state_->resident_buffer_bytes = 0;
  state_->virtuals.Clear(); state_->reflections.clear(); state_->dirty.clear(); state_->samplers.clear();
}
// Accounting follows named cache ownership, not GPU completion. Retained
// command buffers own evicted generations until the bounded frame ring retires.
void ResourceStore::State::EraseTexture(uint32_t handle) {
  InvalidateBindings();
  preparation.ForgetPublished(handle);
  preparation.Invalidate(handle);
  auto found = textures.find(handle);
  if (found == textures.end()) return;
  resident_texture_bytes -= found->second->image.allocatedSize;
  texture_lru.erase(found->second->recency);
  textures.erase(found);
}
void ResourceStore::State::EraseSurface(uint32_t handle) {
  auto found = surfaces.find(handle);
  if (found == surfaces.end()) return;
  gta4_native::GuestSurfaceView view{};
  if (!found->second->depth && gta4_native::DecodeGuestSurfaceView(found->second->descriptor, false, view)) {
    auto group = color_views.find(gta4_native::GetGuestPlacementKey(view));
    if (group != color_views.end()) {
      std::erase(group->second, handle);
      if (group->second.empty()) color_views.erase(group);
    }
  }
  resident_texture_bytes -= found->second->image.allocatedSize;
  surfaces.erase(found);
}
void ResourceStore::State::InstallTexture(uint32_t handle, std::shared_ptr<TextureResource> value) {
  InvalidateBindings();
  EraseTexture(handle);
  texture_lru.push_back(handle);
  value->recency = std::prev(texture_lru.end());
  textures.emplace(handle, value);
  resident_texture_bytes += value->image.allocatedSize;
}
void ResourceStore::State::InstallSurface(uint32_t handle, std::shared_ptr<SurfaceResource> value) {
  EraseSurface(handle);
  surfaces.emplace(handle, value);
  gta4_native::GuestSurfaceView view{};
  if (!value->depth && gta4_native::DecodeGuestSurfaceView(value->descriptor, false, view))
    color_views[gta4_native::GetGuestPlacementKey(view)].push_back(handle);
  resident_texture_bytes += value->image.allocatedSize;
}
void ResourceStore::State::EraseBuffer(uint32_t handle) {
  auto found = buffers.find(handle);
  if (found == buffers.end()) return;
  captured_buffer_bytes -= found->second.payload.size();
  resident_buffer_bytes -= found->second.indices.allocatedSize;
  EraseBufferConversions(found->second);
  buffer_lru.erase(found->second.recency);
  buffers.erase(found);
}
// Error-only inspection. Placement fields are diagnostic hints here, not
// resource identities or a policy for selecting another surface's contents.
std::string ResourceStore::DescribeSurfaceForDiagnostics(const gta4_native::SurfaceDescriptor& source) const {
  std::string result;
  size_t count = 0;
  for (const auto& [handle, candidate] : state_->surfaces) {
    const auto& d = candidate->descriptor;
    if (d.address != source.address || ++count > 12) continue;
    const auto* record = state_->virtuals.Find(handle);
    result += fmt::format(" [{} addr={:08X} base={:08X} guest={}x{}:{} host={}x{}:{} content={} serial={} wrapper={:08X} companion={:08X}]",
        handle, d.address, d.base, d.width, d.height, d.sample_type, candidate->image.width,
        candidate->image.height, candidate->image.sampleCount, candidate->content_mask,
        candidate->content_serial, record ? record->wrapper : 0, record ? record->companion : 0);
  }
  return result;
}
size_t ResourceStore::texture_bytes() const { return state_->resident_texture_bytes; }
ResourceStore::VertexConversionStatistics ResourceStore::vertex_conversion_statistics() const {
  return {state_->vertex_conversion_bytes, state_->vertex_conversion_budget_bytes,
          state_->vertex_conversion_budget, state_->vertex_conversion_lru.size(),
          state_->vertex_conversion_hits, state_->vertex_conversion_misses,
          state_->vertex_conversion_evictions};
}
ResourceStore::TextureCacheStatistics ResourceStore::texture_cache_statistics() const {
  return {state_->resident_texture_bytes, state_->texture_budget, state_->textures.size(),
          state_->texture_evictions, state_->texture_rejections};
}
}  // namespace rex::graphics::gta4_metal
