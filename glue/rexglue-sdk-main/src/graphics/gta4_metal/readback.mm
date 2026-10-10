#include "renderer_state.h"
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/pipeline/texture/conversion.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace rex::graphics::gta4_metal {

bool Renderer::State::Readback(const gta4_native::TextureLockCommand& lock,
    gta4_native::TextureLockResult& result, std::string& error) {
  using namespace gta4_native;
  result = {};
  if (!MaterializePendingClears(error)) return false;
  if (!lock.texture || lock.level >= xenos::kTextureMaxMips) {
    error = "Invalid texture readback request"; return false;
  }
  auto resource = resources.FindTexture(lock.texture);
  const auto* virtual_resource = resources.Virtual(lock.texture);
  if (virtual_resource && virtual_resource->packed_depth_source) {
    const auto header = memory.Read(lock.texture, 52);
    if (header.empty() || !Begin(error)) { if (error.empty()) error = "Unmapped alias texture header"; return false; }
    std::array<uint32_t, 6> words{};
    for (size_t i = 0; i < words.size(); ++i) words[i] = GuestWord(header, 28 + i * sizeof(uint32_t));
    resource = PrepareTexture(lock.texture, std::bit_cast<xenos::xe_gpu_texture_fetch_t>(words), error);
    if (!resource) return false;
  }
  if (!resource) return true;  // No host-produced contents exist for a CPU texture.
  result.generation = resource->generation;
  if (!resource->gpu_produced) return true;
  ProfileRead(resource,5);
  if (!SettlePendingResolves(resource.get(), error)) return false;
  const auto& info = resource->info;
  const auto image = resource->image;
  const bool volume = info.dimension == xenos::DataDimension::k3D;
  const uint32_t layers = info.dimension == xenos::DataDimension::kCube ? 6u : info.is_stacked ? info.depth + 1 : 1;
  if (volume || lock.dimension != TextureLockDimension::k2D || lock.array_index >= layers ||
      lock.level >= image.mipmapLevelCount || lock.level < info.mip_min_level) {
    error = "Unsupported or out-of-range native texture readback subresource"; return false;
  }
  const uint64_t subresource = (uint64_t(lock.level) << 32) | lock.array_index;
  const auto& defined = resource->initialized_subresources;
  if (!resource->subresource_writes.contains(subresource) &&
      std::find(defined.begin(), defined.end(), subresource) == defined.end()) {
    error = "Texture lock requests an undefined subresource"; return false;
  }
  if (virtual_resource && (virtual_resource->guest_backing_width != virtual_resource->logical_width ||
      virtual_resource->guest_backing_height != virtual_resource->logical_height)) {
    error = "Cannot read back a logical render target into a small guest placeholder"; return false;
  }
  const bool depth = image.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
  const auto* format = info.format_info();
  if (!format || format->block_width != 1 || format->block_height != 1 ||
      !std::has_single_bit(format->bytes_per_block())) {
    error = "Unsupported GPU-produced readback storage format"; return false;
  }
  const uint32_t block_bytes = format->bytes_per_block();
  uint32_t width = 0, height = 0, packed_x = 0, packed_y = 0;
  info.GetMipSize(lock.level, &width, &height);
  const size_t row_pitch = (size_t(width) * (depth ? sizeof(float) : block_bytes) + 255u) & ~size_t(255u);
  const size_t stencil_pitch = (size_t(width) + 255u) & ~size_t(255u);
  const size_t color_size = row_pitch * height;
  const size_t size = color_size + (depth ? stencil_pitch * height : 0);
  constexpr size_t maximum_readback = 64u * 1024u * 1024u;
  if (!width || !height || !size || size > maximum_readback) {
    error = "Texture readback exceeds its bounded staging capacity"; return false;
  }
  const auto layout = texture_util::GetGuestTextureLayout(info.dimension, info.pitch >> 5,
      info.width + 1, info.height + 1, info.depth + 1, info.is_tiled, info.format,
      info.has_packed_mips, info.memory.base_address != 0, info.mip_max_level);
  const auto& level = lock.level == 0 ? layout.base : layout.mips[lock.level];
  const uint64_t base_address = uint64_t(info.GetMipLocation(lock.level, &packed_x, &packed_y, true)) +
      uint64_t(lock.array_index) * level.array_slice_stride_bytes;
  const auto extent = info.GetMipExtent(lock.level, true);
  const auto offset_for = [&](uint32_t x, uint32_t y) -> int64_t {
    return info.is_tiled ? texture_util::GetTiledOffset2D(packed_x + x, packed_y + y,
        extent.block_pitch_h, std::countr_zero(block_bytes)) :
        int64_t((uint64_t(packed_y + y) * extent.block_pitch_h + packed_x + x) * block_bytes);
  };
  // Validate the whole write footprint before submitting work or mutating guest bytes.
  uint64_t required_bytes = 0;
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    const int64_t offset = offset_for(x, y);
    if (offset < 0) { error = "Negative tiled readback offset"; return false; }
    required_bytes = std::max(required_bytes, uint64_t(offset) + block_bytes);
  }
  if (required_bytes > maximum_readback || base_address + required_bytes > 0x20000000ull) {
    error = "Texture readback guest address exceeds its physical memory bounds"; return false;
  }
  auto destination = memory.Writable(uint32_t(base_address), size_t(required_bytes), true);
  if (destination.empty()) { error = "Texture readback destination is not writable"; return false; }
  if (!Begin(error)) return false;
  EndRender();
  auto source = image;
  NSUInteger source_level = lock.level, source_slice = lock.array_index;
  const uint32_t physical_width = std::max(1u, uint32_t(image.width) >> lock.level);
  const uint32_t physical_height = std::max(1u, uint32_t(image.height) >> lock.level);
  if (width != physical_width || height != physical_height) {
    if (depth) { error = "Scaled depth readback needs a defined depth reconstruction rule"; return false; }
    auto view = [image newTextureViewWithPixelFormat:image.pixelFormat textureType:MTLTextureType2D
        levels:NSMakeRange(lock.level, 1) slices:NSMakeRange(lock.array_index, 1)];
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:image.pixelFormat width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    source = [context->device newTextureWithDescriptor:descriptor];
    if (!view || !source || !CopyColor(view, source, error)) {
      if (error.empty()) error = "Scaled color readback allocation failed"; return false;
    }
    source_level = source_slice = 0;
  }
  // A previous use has completed at the end of this synchronous method.
  if (!readback_buffer || readback_buffer.length < size) {
    readback_buffer = [context->device newBufferWithLength:size options:MTLResourceStorageModeShared];
    if (!readback_buffer) { error = "Readback buffer allocation failed"; return false; }
    readback_buffer.label = @"Liberty synchronous texture readback";
  }
  auto encoder = [commands blitCommandEncoder];
  if (!encoder) { error = "Texture readback encoder creation failed"; return false; }
  encoder.label = @"Liberty texture lock readback";
  [encoder copyFromTexture:source sourceSlice:source_slice sourceLevel:source_level
      sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
      toBuffer:readback_buffer destinationOffset:0 destinationBytesPerRow:row_pitch destinationBytesPerImage:color_size
      options:depth ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
  if (depth) [encoder copyFromTexture:source sourceSlice:source_slice sourceLevel:source_level
      sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
      toBuffer:readback_buffer destinationOffset:color_size destinationBytesPerRow:stencil_pitch
      destinationBytesPerImage:stencil_pitch * height options:MTLBlitOptionStencilFromDepthStencil];
  [encoder endEncoding];
  if (!Flush(true, error)) return false;
  const auto* bytes = static_cast<const uint8_t*>(readback_buffer.contents);
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    uint32_t packed = 0;
    const uint8_t* source_bytes = bytes + size_t(y) * row_pitch + size_t(x) * block_bytes;
    if (depth) {
      float value; std::memcpy(&value, source_bytes, sizeof(value));
      const uint32_t quantized = info.format == xenos::TextureFormat::k_24_8_FLOAT
          ? xenos::Float32To20e4(value, false)
          : uint32_t(std::nearbyint(std::clamp(double(value), 0.0, 1.0) * 16777215.0));
      packed = (quantized << 8) | bytes[color_size + size_t(y) * stencil_pitch + x];
      source_bytes = reinterpret_cast<const uint8_t*>(&packed);
    }
    texture_conversion::CopySwapBlock(info.endianness, destination.data() + offset_for(x, y), source_bytes, block_bytes);
  }
  result.copied_to_guest = 1;
  return true;
}
}  // namespace rex::graphics::gta4_metal
