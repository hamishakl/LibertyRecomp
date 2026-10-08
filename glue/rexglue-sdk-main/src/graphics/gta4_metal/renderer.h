#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace rex::memory { class Memory; }
namespace rex::graphics::gta4_native { struct FireTraceContext; }
namespace rex::ui { class Presenter; namespace metal { struct MetalContext; } }
namespace rex::graphics::gta4_metal {

// The graphics-system boundary serializes calls. Guest memory is consumed and
// encoded before a call returns: no second scheduler or deferred guest pointers.
class Renderer {
 public:
  struct Statistics {
    uint64_t pipeline_creations = 0;
    uint64_t declaration_definitions = 0;
    uint64_t declaration_reuses = 0;
    uint64_t draws = 0;
    size_t pipeline_entries = 0;
    uint64_t render_passes_created = 0;
    uint64_t clear_load_folds = 0, clear_materializations = 0;
    uint64_t resolve_skips = 0, resolve_initializations_merged = 0;
    uint64_t guest_constant_skips = 0, guest_constant_audits = 0, guest_constant_mismatches = 0;
    uint64_t state_calls = 0, state_skips = 0, residency_calls = 0, residency_skips = 0;
    uint64_t pipeline_lookups = 0, pipeline_lookup_hits = 0;
    uint64_t skipped_pixel_constant_banks=0,material_binding_hits=0,material_binding_misses=0,resolve_image_exchanges=0;
  };
  // Read on the same serialized owner as Submit; no GPU synchronization.
  Statistics statistics() const;
  Renderer(std::shared_ptr<ui::metal::MetalContext> context, memory::Memory* memory,
           ui::Presenter* presenter);
  ~Renderer();
  bool Initialize(std::string& error, const std::filesystem::path& shader_directory = {});
  bool Submit(std::span<const std::byte> command, std::string& error,
              const gta4_native::FireTraceContext* trace = nullptr);
  bool Execute(std::span<const std::byte> command, std::span<std::byte> result, std::string& error);
  bool Finish(std::string& error);
  // Launch-time pipeline cache (PipelineStore). Call after Initialize, before the title submits commands.
  bool OpenPipelineStore(const std::filesystem::path& cache_root, uint32_t title_id, std::string& error);
  size_t PendingPipelineCount() const;
  // Compiles recorded-but-unarchived pipelines into the binary archive. progress(done, total) returns
  // false to stop early; work done so far is still saved.
  bool PrecompilePipelines(const std::function<bool(uint32_t, uint32_t)>& progress, std::string& error);
 private:
  bool SubmitImpl(std::span<const std::byte> command, std::string& error);
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace rex::graphics::gta4_metal
