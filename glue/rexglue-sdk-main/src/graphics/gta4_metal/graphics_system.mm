#include "graphics_system.h"
#include "renderer.h"

#include <rex/graphics/gta4_native/fire_escape_trace.h>
#include <rex/graphics/gta4_native/gpu_pass_origin.h>
#include <rex/graphics/gta4_native/phone_trace.h>
#include <rex/graphics/gta4_native/tv_trace.h>
#include <rex/graphics/gta4_native/title_commands.h>
#include <rex/graphics/gta4_native/temporal_commands.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/metal/provider.h>
#include <rex/ui/windowed_app_context.h>

#include <cstring>
#include <span>

namespace rex::graphics::gta4_metal {
namespace {
// Diagnostic envelopes are transport metadata, not separate rendering routes.
// Limit nesting and validate every wrapper before admitting the title payload.
bool Unwrap(uint32_t& abi, const void*& data, size_t& size, gta4_native::FireTraceContext* fire = nullptr) {
  using namespace gta4_native;
  if (!data || size < sizeof(CommandHeader)) return false;
  for (unsigned depth = 0; depth < 5; ++depth) {
    if (abi == kTitleCommandAbi) return true;
    if (abi == kGpuPassEnvelopeAbi) {
      GpuPassOrigin origin{};
      if (!UnpackGpuPassEnvelope(data, size, abi, origin)) return false;
    } else if (abi == kFireTraceEnvelopeAbi) {
      FireTraceContext context{};
      if (!UnpackFireTraceEnvelope(data, size, abi, context)) return false;
      if (fire) *fire = context;
    } else if (abi == kTvTraceEnvelopeAbi) {
      TvTraceContext context{};
      if (!UnpackTvTraceEnvelope(data, size, abi, context)) return false;
    } else if (abi == kPhoneTraceEnvelopeAbi) {
      PhoneTraceContext context{};
      if (!UnpackPhoneTraceEnvelope(data, size, abi, context)) return false;
    } else {
      return false;
    }
  }
  return false;
}
}

Gta4MetalGraphicsSystem::Gta4MetalGraphicsSystem() = default;
Gta4MetalGraphicsSystem::~Gta4MetalGraphicsSystem() { Shutdown(); }

X_STATUS Gta4MetalGraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  if (presenter_) return X_STATUS_SUCCESS;
  // The producer must keep the presenter selected when it was constructed.
  if (renderer_) return X_STATUS_INVALID_PARAMETER;
  std::string error;
  if (!provider_) provider_ = ui::metal::MetalProvider::Create(error);
  if (!provider_) {
    REXLOG_ERROR("gta4-metal: {}", error);
    return X_STATUS_UNSUCCESSFUL;
  }
  auto setup = [this] { presenter_ = provider_->CreatePresenter(); };
  if (app_context && !app_context->IsInUIThread()) {
    if (!app_context->CallInUIThreadSynchronous(setup)) return X_STATUS_UNSUCCESSFUL;
  } else {
    setup();
  }
  if (!presenter_) return X_STATUS_UNSUCCESSFUL;
  app_context_ = app_context;
  REXLOG_INFO("gta4-metal: native Metal presentation initialized");
  return X_STATUS_SUCCESS;
}

X_STATUS Gta4MetalGraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* dispatcher,
                                               system::KernelState* kernel) {
  std::lock_guard lock(renderer_mutex_);
  if (renderer_) return X_STATUS_SUCCESS;
  if (!dispatcher || !kernel || !dispatcher->memory()) return X_STATUS_INVALID_PARAMETER;
  std::string error;
  if (!provider_) provider_ = ui::metal::MetalProvider::Create(error);
  if (!provider_) {
    REXLOG_ERROR("gta4-metal: {}", error);
    return X_STATUS_UNSUCCESSFUL;
  }
  auto renderer = std::make_unique<Renderer>(provider_->context(), dispatcher->memory(), presenter_.get());
  if (!renderer->Initialize(error)) {
    REXLOG_ERROR("gta4-metal: title initialization failed: {}", error);
    return X_STATUS_UNSUCCESSFUL;
  }
  renderer_ = std::move(renderer);
  failures_ = 0;
  REXLOG_INFO("gta4-metal: title-command renderer connected; abi={} plugin=standalone-metal",
              gta4_native::kTitleCommandAbi);
  return X_STATUS_SUCCESS;
}

bool Gta4MetalGraphicsSystem::has_presentation() const { return presenter_ != nullptr; }
ui::GraphicsProvider* Gta4MetalGraphicsSystem::provider() const { return provider_.get(); }
ui::Presenter* Gta4MetalGraphicsSystem::presenter() const { return presenter_.get(); }
uint32_t Gta4MetalGraphicsSystem::GetTitleCommandAbi(uint32_t title_id) const {
  return title_id == gta4_native::kTitleId ? gta4_native::kTitleCommandAbi : 0;
}

bool Gta4MetalGraphicsSystem::SubmitTitleCommand(uint32_t title_id, uint32_t abi,
                                                 const void* command, size_t size) {
  gta4_native::FireTraceContext trace{};
  if (title_id != gta4_native::kTitleId || !Unwrap(abi, command, size, &trace)) return false;
  std::lock_guard lock(renderer_mutex_);
  if (!renderer_) return false;
  // The game may submit from more than one host thread. Keep instance metadata
  // local to its submitting thread, and apply it under the same renderer lock
  // as its draw so another submitter cannot replace a mesh's temporal identity.
  struct PendingInstance {const void* owner=nullptr;gta4_native::TemporalCommand command{};};
  thread_local PendingInstance temporal;
  gta4_native::CommandHeader header{};std::memcpy(&header,command,sizeof(header));
  if(header.type==gta4_native::CommandType::kTemporalUpdate&&size==sizeof(gta4_native::TemporalCommand)){
    gta4_native::TemporalCommand value;std::memcpy(&value,command,sizeof(value));
    if(value.event==gta4_native::TemporalEvent::kInstance){temporal={this,value};return true;}
  }
  if(header.type==gta4_native::CommandType::kDeviceCreated||header.type==gta4_native::CommandType::kDeviceDestroyed)temporal={};
  std::string error;
  const bool draw=header.type==gta4_native::CommandType::kDrawPrimitive||header.type==gta4_native::CommandType::kDrawPrimitiveUp||header.type==gta4_native::CommandType::kDrawIndexedPrimitive;
  if(draw&&temporal.owner==this&&!renderer_->Submit(std::as_bytes(std::span(&temporal.command,1)),error))return false;
  const bool success = renderer_->Submit({static_cast<const std::byte*>(command), size}, error, &trace);
  if (!success) {
    gta4_native::CommandHeader header{};
    if (size >= sizeof(header)) std::memcpy(&header, command, sizeof(header));
    ++failures_;
    if (failures_ <= 64 || failures_ % 4096 == 0)
      REXLOG_ERROR("gta4-metal: command rejected type={} count={} reason={}",
                    uint32_t(header.type), failures_, error);
  }
  return success;
}

bool Gta4MetalGraphicsSystem::ExecuteTitleCommand(uint32_t title_id, uint32_t abi,
    const void* command, size_t size, void* result, size_t result_size) {
  if (title_id != gta4_native::kTitleId || !result || !Unwrap(abi, command, size)) return false;
  std::lock_guard lock(renderer_mutex_);
  if (!renderer_) return false;
  std::string error;
  const bool success = renderer_->Execute({static_cast<const std::byte*>(command), size},
      {static_cast<std::byte*>(result), result_size}, error);
  if (!success) REXLOG_ERROR("gta4-metal: synchronous command rejected: {}", error);
  return success;
}

void Gta4MetalGraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                      uint32_t title_id, bool blocking) {
  (void)blocking;
  std::lock_guard lock(renderer_mutex_);
  if (!renderer_) return;
  std::string error;
  if (!renderer_->OpenPipelineStore(cache_root, title_id, error))
    REXLOG_WARN("gta4-metal-pipeline-cache: disabled: {}", error);
}

bool Gta4MetalGraphicsSystem::BeginShaderPrecompile(std::function<void()> on_complete) {
  size_t pending = 0;
  {
    std::lock_guard lock(renderer_mutex_);
    if (renderer_) pending = renderer_->PendingPipelineCount();
  }
  if (!pending || precompile_thread_.joinable()) return false;
  precompile_completed_ = 0;
  precompile_total_ = uint32_t(pending);
  precompile_active_ = true;
  precompile_thread_ = std::thread([this, on_complete = std::move(on_complete)] {
    {
      // The title thread is still suspended; holding the producer lock keeps the renderer exclusive.
      std::lock_guard lock(renderer_mutex_);
      std::string error;
      if (renderer_ && !renderer_->PrecompilePipelines(
              [this](uint32_t done, uint32_t total) {
                precompile_completed_ = done;
                precompile_total_ = total;
                return !precompile_cancel_.load();
              },
              error))
        REXLOG_WARN("gta4-metal-pipeline-cache: precompile: {}", error);
    }
    precompile_active_ = false;
    if (on_complete) on_complete();
  });
  return true;
}

system::IGraphicsSystem::ShaderPrecompileProgress Gta4MetalGraphicsSystem::GetShaderPrecompileProgress() const {
  return {precompile_completed_.load(), precompile_total_.load(), precompile_active_.load()};
}

void Gta4MetalGraphicsSystem::Shutdown() {
  precompile_cancel_ = true;
  if (precompile_thread_.joinable()) precompile_thread_.join();
  // Wake publication waits before taking the producer lock during shutdown.
  if (presenter_) presenter_->CancelFramePacingWaits();
  {
    std::lock_guard lock(renderer_mutex_);
    renderer_.reset();
  }
  if (presenter_) {
    auto destroy = [this] { presenter_.reset(); };
    if (app_context_ && !app_context_->IsInUIThread()) {
      if (!app_context_->CallInUIThreadSynchronous(destroy)) {
        REXLOG_ERROR("gta4-metal: UI context unavailable during presentation shutdown");
        return;
      }
    } else {
      destroy();
    }
  }
  provider_.reset();
  app_context_ = nullptr;
}
}  // namespace rex::graphics::gta4_metal
