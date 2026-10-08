#include "resource_state.h"
#include "gpu_pass_timer.h"
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <algorithm>
#include <bit>
#include <cstring>

namespace rex::graphics::gta4_metal {
namespace {
bool IsDepth(MTLPixelFormat format) {return format==MTLPixelFormatDepth32Float_Stencil8;}

}

void ResourceStore::PrefetchTexture(uint32_t handle, const xenos::xe_gpu_texture_fetch_t& fetch) {
  if (!handle || state_->virtuals.Find(handle) || state_->reflections.contains(handle) ||
      (state_->vector_fonts && state_->font_ids.contains(handle))) return;
  if (auto found = state_->textures.find(handle); found != state_->textures.end()) {
    if (found->second->gpu_produced || (!state_->dirty.contains(handle) &&
        gta4_native::NativeTextureImageFetchEqual(found->second->fetch, fetch))) return;
  }
  state_->preparation.Prefetch(handle, fetch, state_->memory);
}
ResourceStore::PreparationStatistics ResourceStore::preparation_statistics() const {
  const auto s = state_->preparation.statistics();
  return {s.queued, s.ready, s.waited, s.inline_decodes, s.staging_allocations,
      s.staging_reuses, s.pending_bytes, s.staging_bytes};
}

bool ResourceStore::InitializeTextureStorage(const std::shared_ptr<TextureResource>& resource,
    id<MTLCommandBuffer> commands, const std::function<void()>& end_render, std::string& error) {
  if (!resource || !resource->image || !resource->gpu_produced || !commands || !end_render) {
    error = "Texture initialization requires an owned target and submission"; return false;
  }
  if (resource->storage_initialized) return true;
  const auto image = resource->image;
  const bool depth = IsDepth(image.pixelFormat), volume = image.textureType == MTLTextureType3D;
  if (!(image.usage & MTLTextureUsageRenderTarget) || (depth && volume)) {
    error = "Texture storage cannot use an attachment clear"; return false;
  }
  end_render();
  for (NSUInteger level = 0; level < image.mipmapLevelCount; ++level) {
    const NSUInteger planes = volume ? std::max(NSUInteger{1}, image.depth >> level) :
        image.textureType == MTLTextureTypeCube ? 6 : image.arrayLength;
    for (NSUInteger plane = 0; plane < planes; ++plane) {
      const uint64_t key = (uint64_t(level) << 32) | plane;
      const auto& defined = resource->initialized_subresources;
      if (resource->subresource_writes.contains(key) ||
          std::find(defined.begin(), defined.end(), key) != defined.end()) continue;
      auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
      if (depth) {
        pass.depthAttachment.texture = pass.stencilAttachment.texture = image;
        pass.depthAttachment.level = pass.stencilAttachment.level = level;
        pass.depthAttachment.slice = pass.stencilAttachment.slice = plane;
        pass.depthAttachment.loadAction = pass.stencilAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = pass.stencilAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 0; pass.stencilAttachment.clearStencil = 0;
      } else {
        auto attachment = pass.colorAttachments[0];
        attachment.texture = image; attachment.level = level;
        if (volume) attachment.depthPlane = plane; else attachment.slice = plane;
        attachment.loadAction = MTLLoadActionClear; attachment.storeAction = MTLStoreActionStore;
        attachment.clearColor = MTLClearColorMake(0, 0, 0, 0);
      }
      gpu_pass_timer::Tag(pass,"texture-prep");
      auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
      if (!encoder) { error = "Texture storage initialization failed"; return false; }
      encoder.label = @"Liberty initialize unwritten texture storage";
      [encoder endEncoding];
      resource->initialized_subresources.push_back(key);
    }
  }
  resource->storage_initialized = resource->initialized = true;
  return true;
}

std::shared_ptr<TextureResource> ResourceStore::Texture(uint32_t handle,
    const xenos::xe_gpu_texture_fetch_t& fetch,id<MTLCommandBuffer> commands,
    const std::function<void()>& end_render,std::string& error) {
  @autoreleasepool {
    error.clear();
    if(!commands || !end_render) {error="Texture update requires an active submission"; return {};}
    const auto font = state_->font_ids.find(handle);
    const uint32_t requested_font = state_->vector_fonts && font != state_->font_ids.end() ? font->second : 0;
    auto existing=state_->textures.find(handle);
    if(existing!=state_->textures.end()) {
      auto resource=existing->second;
      if(!resource->gpu_produced&&resource->font_id==requested_font&&state_->dirty.contains(handle)&&
          !state_->virtuals.Find(handle)&&!state_->reflections.contains(handle)&&
          gta4_native::NativeTextureImageFetchEqual(resource->fetch,fetch)&&state_->preparation.MatchesPublished(handle,fetch,state_->memory)){
        state_->dirty.erase(handle);state_->preparation.Invalidate(handle);state_->TouchTexture(*resource);return resource;
      }
      TextureInfo requested{};
      const bool gpu_shape=resource->gpu_produced && TextureInfo::Prepare(fetch,&requested) &&
          requested.dimension==resource->info.dimension && requested.format==resource->info.format &&
          requested.width==resource->info.width && requested.height==resource->info.height &&
          requested.depth==resource->info.depth;
      if(gpu_shape || (resource->font_id == requested_font && !state_->dirty.contains(handle) && gta4_native::NativeTextureImageFetchEqual(resource->fetch,fetch))) {
        if (resource->gpu_produced && !resource->storage_initialized &&
            !InitializeTextureStorage(resource, commands, end_render, error)) return {};
        state_->TouchTexture(*resource);
        resource->use_serial=++state_->serial; return resource;
      }
    }
    const auto* virtual_resource=state_->virtuals.Find(handle);
    const bool host_owned = (virtual_resource && !virtual_resource->guest_write_conflict) ||
        state_->reflections.contains(handle);
    if(virtual_resource && virtual_resource->guest_write_conflict &&
        (virtual_resource->guest_backing_width!=virtual_resource->logical_width ||
         virtual_resource->guest_backing_height!=virtual_resource->logical_height)) {
      error="Virtual placeholder has no complete CPU texture backing"; return {};
    }
    if(virtual_resource && virtual_resource->packed_depth_source) {
      error="Packed depth alias needs an explicit GPU conversion"; return {};
    }
    if (requested_font && !host_owned) {
      auto font_texture = state_->FontTexture(handle, requested_font, fetch, commands, end_render, error);
      if (font_texture) {
        state_->InstallTexture(handle, font_texture); state_->dirty.erase(handle);
        return font_texture;
      }
      if (!error.empty()) return {};
    }
    auto resource=state_->AllocateTexture(handle,fetch,host_owned,error);
    if(!resource) return {};
    resource->font_id = requested_font;
    const auto& info=resource->info;
    const bool depth=IsDepth(resource->image.pixelFormat);
    const bool is_3d=info.dimension==xenos::DataDimension::k3D;
    const uint32_t layers=info.dimension==xenos::DataDimension::kCube ? 6 : info.is_stacked ? info.depth+1 : 1;
    if(host_owned) {
      if (!InitializeTextureStorage(resource, commands, end_render, error)) return {};
    } else {
      auto prepared = state_->preparation.Take(handle, fetch,state_->memory);
      if (prepared) {
        if (!state_->preparation.Wait(prepared, error)) return {};
        end_render();
        auto blit = [commands blitCommandEncoder];
        if (!blit) { error = "Prepared texture upload encoder creation failed"; return {}; }
        blit.label = @"Liberty prepared texture upload";
        bool success = true;
        for (const auto& slice : prepared->slices) {
          auto staging = state_->preparation.Staging(state_->context->device, commands, slice.bytes.size());
          if (!staging) { error = "Prepared texture upload allocation failed"; success = false; break; }
          std::memcpy(staging.data,slice.bytes.data(),slice.bytes.size());
          [blit copyFromBuffer:staging.buffer sourceOffset:staging.offset sourceBytesPerRow:slice.row_pitch
              sourceBytesPerImage:slice.image_pitch sourceSize:MTLSizeMake(slice.width, slice.height, slice.depth)
              toTexture:resource->image destinationSlice:slice.layer destinationLevel:slice.mip
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
        [blit endEncoding];
        if (!success) return {};
        resource->initialized = true;
        state_->InstallTexture(handle, resource); state_->dirty.erase(handle);
        state_->preparation.RememberPublished(handle,prepared);return resource;
      }
      // The Vulkan reference also rejects CPU-owned packed depth payloads.
      // Do not reinterpret their packed bytes as native float depth.
      if(depth) {error="CPU-owned packed depth/stencil uploads are unsupported"; return {};}
      const auto base=GetBaseFormat(info.format);
      const auto* guest_format=info.format_info();
      const auto* host_format=base==xenos::TextureFormat::k_DXT3A ? FormatInfo::Get(xenos::TextureFormat::k_DXT2_3) :
          (base==xenos::TextureFormat::k_DXN || base==xenos::TextureFormat::k_CTX1) ? FormatInfo::Get(xenos::TextureFormat::k_8_8) :
          base==xenos::TextureFormat::k_DXT5A ? FormatInfo::Get(xenos::TextureFormat::k_8) : guest_format;
      const uint32_t guest_block=guest_format->bytes_per_block(), host_block=host_format->bytes_per_block();
      if(!std::has_single_bit(guest_block) || !host_block) {error="Unsupported texture block shape"; return {};}
      const auto layout=texture_util::GetGuestTextureLayout(info.dimension,info.pitch>>5,info.width+1,
          info.height+1,info.depth+1,info.is_tiled,info.format,info.has_packed_mips,
          info.memory.base_address!=0,info.mip_max_level);
      end_render();
      auto blit=[commands blitCommandEncoder];
      if(!blit) {error="Texture upload encoder creation failed"; return {};}
      bool succeeded=true;
      for(uint32_t mip=info.mip_min_level;mip<=info.mip_max_level && succeeded;++mip) {
        uint32_t width=0,height=0,packed_x=0,packed_y=0;
        info.GetMipSize(mip,&width,&height);
        const uint32_t depth_count=is_3d ? std::max(1u,(info.depth+1)>>mip) : 1;
        const auto guest_extent=info.GetMipExtent(mip,true);
        const auto& level=mip==0 ? layout.base : layout.mips[mip];
        const uint32_t address=info.GetMipLocation(mip,&packed_x,&packed_y,true);
        const auto source=state_->memory.Read(address,level.level_data_extent_bytes,true);
        if(source.empty()) {error="Unmapped guest texture mip payload"; succeeded=false; break;}
        const bool expand=base==xenos::TextureFormat::k_CTX1 || base==xenos::TextureFormat::k_DXN || base==xenos::TextureFormat::k_DXT5A;
        const uint32_t blocks_x=(width+guest_format->block_width-1)/guest_format->block_width;
        const uint32_t blocks_y=(height+guest_format->block_height-1)/guest_format->block_height;
        const uint32_t storage_width=expand ? blocks_x*guest_format->block_width : width;
        const uint32_t storage_height=expand ? blocks_y*guest_format->block_height : height;
        const size_t row_bytes=((size_t(storage_width)+host_format->block_width-1)/host_format->block_width)*host_block;
        const size_t row_pitch=(row_bytes+255)&~size_t(255);
        const size_t rows=(storage_height+host_format->block_height-1)/host_format->block_height;
        const size_t slice_bytes=row_pitch*rows;
        const size_t total=slice_bytes*depth_count;
        if(!total || total>64u*1024u*1024u) {error="Texture upload exceeds its payload bound"; succeeded=false; break;}
        for(uint32_t layer=0;layer<(is_3d?1:layers) && succeeded;++layer) {
          auto staging=state_->preparation.Staging(state_->context->device, commands, total);
          if(!staging) {error="Texture upload allocation failed"; succeeded=false; break;}
          std::memset(staging.data,0,total);
          auto* destination=static_cast<uint8_t*>(staging.data);
          for(uint32_t z=0;z<depth_count && succeeded;++z) for(uint32_t y=0;y<blocks_y && succeeded;++y)
            for(uint32_t x=0;x<blocks_x;++x) {
              const uint32_t sx=packed_x+x,sy=packed_y+y;
              const int64_t offset=info.is_tiled ? (is_3d ? texture_util::GetTiledOffset3D(sx,sy,z,
                  guest_extent.block_pitch_h,guest_extent.block_pitch_v,std::countr_zero(guest_block)) :
                  texture_util::GetTiledOffset2D(sx,sy,guest_extent.block_pitch_h,std::countr_zero(guest_block))) :
                  int64_t(((uint64_t(z)*guest_extent.block_pitch_v+sy)*guest_extent.block_pitch_h+sx)*guest_block);
              const uint64_t source_offset=uint64_t(layer)*level.array_slice_stride_bytes+uint64_t(offset);
              if(offset<0 || source_offset>source.size() || guest_block>source.size()-source_offset) {
                error="Guest texture block is outside its validated mip span"; succeeded=false; break;
              }
              const auto* input=source.data()+source_offset;
              auto* output=destination+size_t(z)*slice_bytes+
                  (expand ? size_t(y)*guest_format->block_height*row_pitch+size_t(x)*guest_format->block_width*host_block : size_t(y)*row_pitch+size_t(x)*host_block);
              switch(base) {
                case xenos::TextureFormat::k_CTX1: texture_conversion::ConvertTexelCTX1ToR8G8(info.endianness,output,input,row_pitch); break;
                case xenos::TextureFormat::k_DXN: texture_conversion::ConvertTexelDXNToR8G8(info.endianness,output,input,row_pitch); break;
                case xenos::TextureFormat::k_DXT5A: texture_conversion::ConvertTexelDXT5AToR8(info.endianness,output,input,row_pitch); break;
                case xenos::TextureFormat::k_DXT3A: texture_conversion::ConvertTexelDXT3AToDXT3(info.endianness,output,input,host_block); break;
                default: texture_conversion::CopySwapBlock(info.endianness,output,input,host_block); break;
              }
            }
          if(succeeded) [blit copyFromBuffer:staging.buffer sourceOffset:staging.offset sourceBytesPerRow:row_pitch
              sourceBytesPerImage:slice_bytes sourceSize:MTLSizeMake(width,height,depth_count)
              toTexture:resource->image destinationSlice:layer destinationLevel:mip destinationOrigin:MTLOriginMake(0,0,0)];
        }
      }
      [blit endEncoding];
      if(!succeeded) return {};
    }
    resource->initialized=true;
    state_->InstallTexture(handle,resource); state_->dirty.erase(handle); return resource;
  }
}

id<MTLTexture> ResourceStore::View(const std::shared_ptr<TextureResource>& resource,
    const xenos::xe_gpu_texture_fetch_t&, std::string& error) {
  error.clear();
  if (!resource || !resource->image) { error = "Missing texture generation"; return nil; }
  // Match the maintained native Vulkan macOS portability sampling contract:
  // ordinary decoded channels reach the existing shaders through identity views.
  // Applying the guest fetch remap here exchanges diffuse channels and changes
  // packed normal-map components relative to that reference. Packed-depth aliases
  // already receive their explicit layout in PrepareTexture's conversion pass.
  // No per-texture view allocation or secondary channel-remapping cache is needed.
  return resource->font_view ? resource->font_view : resource->image;
}

}  // namespace rex::graphics::gta4_metal
