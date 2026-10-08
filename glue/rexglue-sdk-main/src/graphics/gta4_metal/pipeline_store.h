#pragma once
#import <Metal/Metal.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace rex::graphics::gta4_metal {

// Which shader archive a recorded function is resolved from on replay.
enum class RecipeLibrary : uint8_t { kNone = 0, kStock, kOverride, kTemporalStock, kTemporalOverride, kDepthMotion };

struct RecipeFunction {
  uint64_t hash = 0;
  uint32_t specialization = 0;
  RecipeLibrary library = RecipeLibrary::kNone;
  uint8_t stage = 0, late = 0, isolated_ui = 0;
};

// Everything needed to rebuild one MTLRenderPipelineDescriptor. Unlike the in-memory pipeline Key it
// holds no per-session identities (vertex declarations are captured as their resolved vertex
// descriptor), so a recipe recorded in one launch is replayable in the next.
struct PipelineRecipe {
  static constexpr uint32_t kVersion = 1;
  static constexpr uint32_t kColors = 8, kAttributes = 31, kLayouts = 31;
  uint32_t version = kVersion;
  uint32_t samples = 1;
  uint32_t depth_format = 0, stencil_format = 0;
  RecipeFunction vertex, fragment;
  struct Color {
    uint32_t format = 0, write_mask = 0;
    uint8_t blending = 0, src_rgb = 0, dst_rgb = 0, src_a = 0, dst_a = 0, op_rgb = 0, op_a = 0, pad = 0;
  } colors[kColors];
  struct Attribute { uint32_t format = 0, offset = 0, buffer = 0; } attributes[kAttributes];
  struct Layout { uint32_t stride = 0, step = 0; } layouts[kLayouts];
};
static_assert(std::is_trivially_copyable_v<PipelineRecipe>);

PipelineRecipe RecipeFromDescriptor(MTLRenderPipelineDescriptor* descriptor, const RecipeFunction& vertex,
                                    const RecipeFunction& fragment);
MTLRenderPipelineDescriptor* DescriptorFromRecipe(const PipelineRecipe& recipe, id<MTLFunction> vertex,
                                                  id<MTLFunction> fragment);
uint64_t HashRecipe(const PipelineRecipe& recipe);

// Persists recipes for every pipeline the title builds, and a Metal binary archive of the compiled
// pipelines. Launch-time precompilation adds the recorded-but-unarchived recipes to the archive; at
// runtime every pipeline creation consults the archive, so archived pipelines skip GPU compilation.
//
// <cache_root>/gta4-metal/<title>/recipes-v1.bin                append-only recipe log
// <cache_root>/gta4-metal/<title>/pipelines-<identity>.metalar  MTLBinaryArchive
// <cache_root>/gta4-metal/<title>/pipelines-<identity>.manifest recipe hashes held by that archive
// The identity covers the GPU, the OS build and the shader archives, so any change rebuilds it.
class PipelineStore {
 public:
  bool Open(id<MTLDevice> device, const std::filesystem::path& cache_root, uint32_t title_id,
            uint64_t shader_identity, std::string& error);
  bool is_open() const { return archive_ != nil; }
  id<MTLBinaryArchive> archive() const { return archive_; }

  // Runtime: remember a newly built pipeline (deduplicated, appended to the log).
  void Record(const PipelineRecipe& recipe);
  // Recorded recipes the archive does not contain yet.
  std::vector<PipelineRecipe> Pending() const;
  // Compiles one descriptor into the archive. Not thread-safe with Save().
  bool Add(MTLRenderPipelineDescriptor* descriptor, uint64_t recipe_hash, std::string& error);
  // Marks a recipe that cannot be built with this build's shaders, so it is not retried every launch.
  void Skip(uint64_t recipe_hash);
  bool Save(std::string& error);

 private:
  mutable std::mutex mutex_;
  id<MTLDevice> device_ = nil;
  id<MTLBinaryArchive> archive_ = nil;
  std::filesystem::path directory_, recipes_path_, archive_path_, manifest_path_;
  std::vector<PipelineRecipe> recipes_;
  std::unordered_set<uint64_t> recorded_, archived_;
  std::ofstream log_;
  bool dirty_ = false;
};

}  // namespace rex::graphics::gta4_metal
