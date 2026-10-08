#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <rex/graphics/gta4_native/title_commands.h>
#include "shader_archive.h"

namespace rex::ui::metal { struct MetalContext; }

namespace rex::graphics::gta4_metal {

struct ShaderMetadata {
  uint64_t hash = 0;
  gta4_native::ShaderStage stage = gta4_native::ShaderStage::kPixel;
  uint32_t used_texture_mask = 0;
  uint32_t specialization_constants_mask = 0;
  uint32_t color_output_mask = 0;
  bool writes_depth = false;
  bool late_available = false;
  std::string name;
  uint32_t attribute_count = 0;
  std::array<MetalVertexAttribute, kMetalArchiveMaximumAttributes> attributes{};
  bool native_output = false;
  bool coverage_output = false;
};

// Runtime libraries are mapped only for Metal. Vulkan/DXIL caches stay unopened.
// Function() is a pipeline-construction operation, not a per-draw lookup.
class ShaderCache {
 public:
  explicit ShaderCache(std::shared_ptr<ui::metal::MetalContext> context,
                       std::string resource_name = "title_shader_archive");
  ~ShaderCache();
  ShaderCache(const ShaderCache&) = delete;
  ShaderCache& operator=(const ShaderCache&) = delete;
  bool Initialize(std::string& error);
  bool InitializeFile(const std::string& path, std::string& error);
  std::optional<ShaderMetadata> Lookup(uint64_t hash, gta4_native::ShaderStage stage,
                                       std::string& error);
  // Immutable, archive-owned metadata; stable until Clear(). Per-draw callers
  // borrow it rather than allocate/copy the diagnostic name and attribute bank.
  const ShaderMetadata* Metadata(uint64_t hash,gta4_native::ShaderStage stage,std::string& error);

  id<MTLFunction> Function(uint64_t hash, gta4_native::ShaderStage stage,
                          uint32_t specialization, bool late, std::string& error, bool isolated_ui = false);
  void Clear();
  // Content identity of the mapped archive (0 when not initialized); keys the pipeline binary archive.
  uint64_t Identity() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace rex::graphics::gta4_metal
