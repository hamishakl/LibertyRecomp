#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <rex/system/interfaces/graphics.h>

namespace rex::ui::metal { class MetalProvider; }
namespace rex::graphics::gta4_metal {
class Renderer;

// Title commands are consumed synchronously under one owner lock. Metal queues
// GPU work; the host does not retain guest pointers or introduce another worker.
class Gta4MetalGraphicsSystem final : public system::IGraphicsSystem {
 public:
  Gta4MetalGraphicsSystem();
  ~Gta4MetalGraphicsSystem() override;
  X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) override;
  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* dispatcher, system::KernelState* kernel) override;
  bool has_presentation() const override;
  ui::GraphicsProvider* provider() const override;
  ui::Presenter* presenter() const override;
  uint32_t GetTitleCommandAbi(uint32_t title_id) const override;
  bool SubmitTitleCommand(uint32_t title_id, uint32_t abi, const void* command, size_t size) override;
  bool ExecuteTitleCommand(uint32_t title_id, uint32_t abi, const void* command, size_t size,
                           void* result, size_t result_size) override;
  void Shutdown() override;
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) override;
  bool BeginShaderPrecompile(std::function<void()> on_complete) override;
  ShaderPrecompileProgress GetShaderPrecompileProgress() const override;

 private:
  std::mutex renderer_mutex_;
  std::unique_ptr<ui::metal::MetalProvider> provider_;
  std::unique_ptr<ui::Presenter> presenter_;
  std::unique_ptr<Renderer> renderer_;
  ui::WindowedAppContext* app_context_ = nullptr;
  uint64_t failures_ = 0;
  std::thread precompile_thread_;
  std::atomic<uint32_t> precompile_completed_{0}, precompile_total_{0};
  std::atomic<bool> precompile_active_{false}, precompile_cancel_{false};
};
}  // namespace rex::graphics::gta4_metal
