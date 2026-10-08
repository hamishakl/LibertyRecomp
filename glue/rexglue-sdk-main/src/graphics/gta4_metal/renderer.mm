#include <chrono>
#include <xxhash.h>
#include <tuple>
#include "renderer_state.h"
#include "gpu_pass_timer.h"
#include "../gta4_native/modern_shader_options.h"
#include "metal_native_library.h"
#include <rex/graphics/gta4_native/lighting_semantics.h>
#include <rex/cvar.h>
#include <rex/diagnostics/policy.h>
#include <rex/logging.h>
#include <dispatch/dispatch.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>

REXCVAR_DEFINE_UINT32(gta4_metal_profile_delay_seconds, 0, "GPU/Diagnostics", "Delay before bounded Metal scope records");
REXCVAR_DEFINE_UINT32(gta4_metal_profile_duration_seconds, 0, "GPU/Diagnostics", "Opt-in Metal scope record interval, at most thirty seconds");
REXCVAR_DEFINE_BOOL(gta4_metal_exchange_resolve_clear,true,"GPU","Transfer a full resolve image when its original source is fully cleared");
REXCVAR_DEFINE_BOOL(gta4_metal_cache_material_bindings,true,"GPU","Reuse prepared immutable asset texture/sampler bindings");
REXCVAR_DEFINE_BOOL(gta4_metal_cache_constant_sources, true, "GPU", "Keep bounded immutable CPU constant versions");
REXCVAR_DEFINE_BOOL(gta4_metal_track_guest_constants, true, "GPU", "Reuse float banks while the guest reports no changes");
REXCVAR_DEFINE_BOOL(gta4_metal_audit_guest_constants, false, "GPU/Diagnostics", "Compare every skipped constant bank against live guest memory");
REXCVAR_DEFINE_BOOL(gta4_metal_cache_pipeline_lookup, true, "GPU", "Reuse recent exact pipeline keys before the full cache lookup");
REXCVAR_DEFINE_BOOL(gta4_metal_cache_encoder_state, true, "GPU", "Reuse unchanged state in one Metal encoder");
REXCVAR_DEFINE_BOOL(gta4_metal_foreground_qos, true, "GPU", "Bound recording work to foreground QoS");
REXCVAR_DEFINE_BOOL(gta4_metal_fold_full_clears, true, "GPU", "Fold full clears into the next attachment use");
REXCVAR_DEFINE_BOOL(gta4_metal_retain_ignore_address, true, "GPU", "Keep colour-masked draws in the current render pass when only the guest surface address differs");
REXCVAR_DEFINE_STRING(gta4_metal_gpu_pass_log, "", "GPU/Diagnostics", "Append average GPU ms per render-pass category to this CSV every 300 frames (empty = off)");
REXCVAR_DEFINE_STRING(gta4_metal_frame_log, "", "GPU/Diagnostics", "Write per-frame Metal timing CSV here at shutdown (empty = off)");
REXCVAR_DEFINE_BOOL(gta4_metal_pipeline_archive, true, "GPU", "Record pipelines and precompile them into a Metal binary archive at launch");
REXCVAR_DEFINE_BOOL(gta4_metal_async_pipelines, true, "GPU", "Overlap title pipeline creation with draw resource preparation");
REXCVAR_DEFINE_BOOL(gta4_metal_prepare_textures, true, "GPU", "Decode owned texture snapshots on bounded workers");
REXCVAR_DEFINE_STRING(gta4_metal_capture_path, "", "GPU/Diagnostics", "Optional Xcode .gputrace destination for one title submission");
REXCVAR_DEFINE_UINT32(gta4_metal_capture_delay_seconds, 60, "GPU/Diagnostics", "Delay before the opt-in Xcode Metal GPU capture");
REXCVAR_DEFINE_BOOL(gta4_metal_capture_autostart, true, "GPU/Diagnostics", "Automatically arm the optional Xcode capture after its delay");
namespace { std::atomic<bool> g_metal_capture_requested{false}; }
extern "C" __attribute__((visibility("default"))) int rex_gta4_metal_capture_start() {
  g_metal_capture_requested.store(true, std::memory_order_release); return 1;
}
namespace rex::graphics::gta4_metal {
namespace {
template<class T> T Command(std::span<const std::byte> bytes) {
  T value{}; std::memcpy(&value,bytes.data(),sizeof(value)); return value;
}
}
Renderer::State::State(std::shared_ptr<ui::metal::MetalContext> c,memory::Memory* m,ui::Presenter* p)
    :context(std::move(c)),memory(m),presenter(p),resources(context,memory),stock(context),
     overrides(context,"override_shader_archive"),temporal_stock(context,"stock_temporal_shader_archive"),
     temporal_overrides(context,"override_temporal_shader_archive"),frames(64u*1024u*1024u,
       rex::cvar::Query<uint32_t>("gta4_native_frames_in_flight")) {}
Renderer::Renderer(std::shared_ptr<ui::metal::MetalContext> context,memory::Memory* memory,ui::Presenter* presenter)
    :state_(std::make_unique<State>(std::move(context),memory,presenter)) {}
Renderer::~Renderer() {std::string error; if(!Finish(error)) REXLOG_ERROR("gta4-metal: shutdown: {}",error); state_->WriteFrameLog();}

bool Renderer::Initialize(std::string& error,const std::filesystem::path& directory) {
  @autoreleasepool {
    error.clear(); auto& s=*state_;
    if(s.ready) return true;
    if(!s.context || !s.context->device || s.context->device.argumentBuffersSupport!=MTLArgumentBuffersTier2) {
      error="Native Metal title rendering requires Tier 2 argument buffers"; return false;
    }
    if(directory.empty()) {
      if(!s.stock.Initialize(error) || !s.overrides.Initialize(error)) return false;
    } else if(!s.stock.InitializeFile((directory/"title_shader_archive.bin").string(),error) ||
              !s.overrides.InitializeFile((directory/"override_shader_archive.bin").string(),error)) return false;
    if(!s.context->pass_library) {
      NSURL* url=directory.empty() ? [[NSBundle mainBundle] URLForResource:@"passes" withExtension:@"metallib" subdirectory:@"metal"] :
          [NSURL fileURLWithPath:[NSString stringWithUTF8String:(directory/"passes.metallib").c_str()]];
      NSError* native_error=nil;
      s.context->pass_library=[s.context->device newLibraryWithURL:url error:&native_error];
      if(!s.context->pass_library) {error=ui::metal::MetalError(native_error,"Native utility library is missing"); return false;}
    }
    auto data=dispatch_data_create(kMetalNativeLibrary,sizeof(kMetalNativeLibrary),nullptr,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* native_error=nil;
    s.native_library=[s.context->device newLibraryWithData:data error:&native_error];
    if(!s.native_library) {error=ui::metal::MetalError(native_error,"Native operation library loading failed"); return false;}
    const MTLTextureType types[]={MTLTextureType2D,MTLTextureType2DArray,MTLTextureType3D,MTLTextureTypeCube};
    const std::array<uint8_t,4> zero{};
    for(size_t i=0;i<s.fallback_textures.size();++i) {
      auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
      descriptor.textureType=types[i]; descriptor.storageMode=MTLStorageModeShared;
      descriptor.usage=MTLTextureUsageShaderRead;
      auto image=[s.context->device newTextureWithDescriptor:descriptor];
      if(!image) {error="Fallback texture allocation failed"; return false;}
      for(NSUInteger face=0;face<(types[i]==MTLTextureTypeCube?6:1);++face)
        [image replaceRegion:MTLRegionMake3D(0,0,0,1,1,1) mipmapLevel:0 slice:face
            withBytes:zero.data() bytesPerRow:zero.size() bytesPerImage:zero.size()];
      image.label=@"Liberty unbound texture"; s.fallback_textures[i]=image;
    }
    auto sampler=[MTLSamplerDescriptor new]; sampler.supportArgumentBuffers=YES;
    sampler.minFilter=sampler.magFilter=MTLSamplerMinMagFilterNearest;
    sampler.sAddressMode=sampler.tAddressMode=MTLSamplerAddressModeClampToEdge;
    s.fallback_sampler=[s.context->device newSamplerStateWithDescriptor:sampler];
    s.default_vertex=[s.context->device newBufferWithLength:16 options:MTLResourceStorageModeShared];
    if(!s.fallback_sampler || !s.default_vertex) {error="Default binding allocation failed"; return false;}
    std::memset(s.default_vertex.contents,0,s.default_vertex.length);
    s.override_mode=gta4_native::ParseShaderOverrideMode(rex::cvar::GetFlagByName("gta4_native_light_overrides"));
    s.anti_aliasing = gta4_native::ResolveAntiAliasingConfiguration(
        rex::cvar::GetFlagByName("gta4_native_anti_aliasing"),
        rex::cvar::GetFlagByName("gta4_native_msaa"),
        rex::cvar::Query<bool>("gta4_native_spatial_aa"),
        rex::cvar::Query<bool>("gta4_native_anti_aliasing_unified")).mode;
    s.temporal_upscale=rex::cvar::GetFlagByName("gta4_native_upscaler")=="metalfx";
    s.temporal_enabled=gta4_native::UsesTemporalAntiAliasing(s.anti_aliasing)||s.temporal_upscale;
    s.temporal_generation=s.temporal_enabled&&rex::cvar::Query<bool>("gta4_metalfx_frame_generation");
    if(s.temporal_upscale)s.anti_aliasing=gta4_native::AntiAliasingMode::kMetalFxTaa;
    if(s.temporal_enabled){
      if(directory.empty()){
        if(!s.temporal_stock.Initialize(error)||!s.temporal_overrides.Initialize(error))return false;
      }else if(!s.temporal_stock.InitializeFile((directory/"stock_temporal_shader_archive.bin").string(),error)||
               !s.temporal_overrides.InitializeFile((directory/"override_temporal_shader_archive.bin").string(),error))return false;
      if(!s.temporal_scene.Initialize(s.context,error))return false;
      REXLOG_INFO("gta4-metal-temporal: enabled method={} upscale={} frame-generation={}",gta4_native::AntiAliasingModeName(s.anti_aliasing),s.temporal_upscale,s.temporal_generation);
    }
    if (!s.post_processing.Initialize(s.context, error)) return false;
    gpu_pass_timer::Initialize(s.context->device,rex::cvar::GetFlagByName("gta4_metal_gpu_pass_log"));
    s.retain_ignore_address=rex::cvar::Query<bool>("gta4_metal_retain_ignore_address");
    s.frame_log_path=rex::cvar::GetFlagByName("gta4_metal_frame_log");
    if(!s.frame_log_path.empty()) s.frame_samples.reserve(1u<<16);
    s.ready=true;
    REXLOG_INFO("gta4-metal: title renderer initialized; device={} bindings=direct-resource-ids",s.context->device.name.UTF8String);
    return true;
  }
}

bool Renderer::State::Begin(std::string& error) {
  if(commands) return true;
  if(!modern.active()) {
    modern.Begin(gta4_native::ReadModernShaderSettings()); resources.BeginFrame();
    modern_diagnostics.BeginFrame(gta4_native::ModernShaderTraceEnabled(), modern.settings(), "metal");
    const auto requested=gta4_native::ParseAntiAliasingMode(rex::cvar::GetFlagByName("gta4_native_anti_aliasing"));
    if(requested && gta4_native::CanApplyAntiAliasingLive(anti_aliasing,*requested)) anti_aliasing=*requested;
  }
  const bool qos_active = frame_qos.Begin(presenter && rex::cvar::Query<bool>("gta4_metal_foreground_qos"));
  if (!qos_announced && presenter) {
    qos_announced = true;
    REXLOG_INFO("gta4-metal-qos: requested=user-initiated override={} guest-priority-unchanged=true", qos_active);
  }
  frame=frames.Begin(error); if(!frame) {frame_qos.End(); return false;}
  if (!profile_origin_ns) profile_origin_ns = ui::FramePacerNowNs();
  if (!capture_attempted) {
    const auto path = rex::cvar::GetFlagByName("gta4_metal_capture_path");
    const auto delay = uint64_t(rex::cvar::Query<uint32_t>("gta4_metal_capture_delay_seconds")) * 1'000'000'000ull;
    if (!path.empty() &&
        ((rex::cvar::Query<bool>("gta4_metal_capture_autostart") && ui::FramePacerNowNs() - profile_origin_ns >= delay) ||
         g_metal_capture_requested.exchange(false, std::memory_order_acq_rel))) {
      capture_attempted = true;
      auto manager = [MTLCaptureManager sharedCaptureManager];
      auto descriptor = [MTLCaptureDescriptor new];
      descriptor.captureObject = context->device;
      descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
      descriptor.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
      NSError* native_error = nil;
      if ([manager supportsDestination:descriptor.destination] &&
          [manager startCaptureWithDescriptor:descriptor error:&native_error]) {
        capture_active = true; REXLOG_INFO("gta4-metal-capture: started {}", path);
      } else REXLOG_ERROR("gta4-metal-capture: {}", ui::metal::MetalError(native_error,
          "GPU capture unavailable; launch with MTL_CAPTURE_ENABLED=1"));
    }
  }
  commands=[context->queue commandBuffer];
  if(!commands) {frames.Cancel(*frame); frame=nullptr; frame_qos.End(); error="Metal command buffer allocation failed"; return false;}
  commands.label=[NSString stringWithFormat:@"Liberty title batch %llu",submitted];
  if (!profile_origin_ns) profile_origin_ns=ui::FramePacerNowNs();
  const uint64_t profile_elapsed=ui::FramePacerNowNs()-profile_origin_ns;
  const uint64_t profile_delay=uint64_t(rex::cvar::Query<uint32_t>("gta4_metal_profile_delay_seconds"))*1'000'000'000ull;
  const uint64_t profile_duration=uint64_t(std::min(30u,rex::cvar::Query<uint32_t>("gta4_metal_profile_duration_seconds")))*1'000'000'000ull;
  profile_enabled=profile_elapsed>=profile_delay && profile_elapsed-profile_delay<profile_duration &&
      rex::diagnostics::IsEnabled(rex::diagnostics::Category::kNativeProfiler);
  resolve_profile.Begin();
  constants.Clear();
  cache_constant_sources = rex::cvar::Query<bool>("gta4_metal_cache_constant_sources");
  cache_material_bindings=rex::cvar::Query<bool>("gta4_metal_cache_material_bindings");
  track_guest_constants = rex::cvar::Query<bool>("gta4_metal_track_guest_constants");
  audit_guest_constants = rex::cvar::Query<bool>("gta4_metal_audit_guest_constants");
  guest_constants.Reset();
  cache_encoder_state = rex::cvar::Query<bool>("gta4_metal_cache_encoder_state");
  cache_pipeline_lookup = rex::cvar::Query<bool>("gta4_metal_cache_pipeline_lookup");
  if (!cache_pipeline_lookup) pipeline_lookup.Reset();
  for (auto& bank : constant_banks) { bank.size = 0; bank.upload = {};bank.source_view=nullptr; }
  return true;
}
void Renderer::State::EndRender(std::source_location caller) {
  if(render && gpu_pass_timer::enabled()) {
    const char* file=caller.file_name(); if(const char* slash=std::strrchr(file,'/')) file=slash+1;
    gpu_pass_timer::Count(fmt::format("end-pass@{}:{}",file,caller.line()));
  }
  if(render) { FinishScopeProfile(); [render endEncoding]; render=nil; bindings.Reset(cache_encoder_state); }
  active_targets={};
}
bool Renderer::State::Flush(bool wait,std::string& error) {
  if (!MaterializePendingClears(error)) { frame_qos.End(); return false; }
  EndRender();
  FinishResolveProfile();
  if(!commands) frame_qos.End();
  if(!commands) return !wait || frames.WaitIdle(error);
  auto committed=commands;
  if(modern_diagnostics.enabled()) {
    const auto diagnostic_frame=modern_diagnostics.frame(), command=diagnostic_command, submission=submitted;
    [committed addCompletedHandler:^(id<MTLCommandBuffer> completed) {
      if(completed.status!=MTLCommandBufferStatusCompleted)
        REXLOG_ERROR("gta4-modern-exec: backend=metal event=gpu-submission-failed trace-frame={} "
            "last-command={} submission={} status={} reason={}",diagnostic_frame,command,submission,
            uint32_t(completed.status),ui::metal::MetalError(completed.error,"GPU submission failed"));
    }];
  }
  if(!frame_log_path.empty()) {
    auto gpu_total=frame_gpu_ns;
    [committed addCompletedHandler:^(id<MTLCommandBuffer> completed) {
      const double seconds=completed.GPUEndTime-completed.GPUStartTime;
      if(seconds>0) gpu_total->fetch_add(uint64_t(seconds*1e9));
    }];
  }
  gpu_pass_timer::Commit(committed);
  if(!frames.Commit(*frame,committed)) {frame_qos.End(); error="Metal frame submission failed"; return false;}
  if (capture_active) {
    [[MTLCaptureManager sharedCaptureManager] stopCapture]; capture_active = false;
    REXLOG_INFO("gta4-metal-capture: saved title submission {}", submitted);
  }
  frame_qos.End();
  commands=nil; frame=nullptr; constants.Clear(); ++submitted;
  if(wait) {
    [committed waitUntilCompleted];
    if(committed.status!=MTLCommandBufferStatusCompleted) {error=ui::metal::MetalError(committed.error,"Metal GPU submission failed"); return false;}
  }
  return true;
}
ui::metal::UploadSlice Renderer::State::Upload(std::span<const uint8_t> bytes, bool swap, std::string& error) {
  if (bytes.empty() || !Begin(error)) return {};
  if (swap && bytes.size() % sizeof(uint32_t)) {
    error = "Constant bank is not word-aligned"; return {};
  }
  ConstantBank* bank = swap && bytes.size() == 0x1000 ? &constant_banks[0] :
                       swap && bytes.size() == 0xE00 ? &constant_banks[1] :
                       cache_constant_sources && !swap && bytes.size() == sizeof(gta4_native::core::SharedConstants) && bytes.size() <= 4096 ? &constant_banks[2] :
                       cache_constant_sources && !swap && bytes.size() == 5u*gta4_native::kTextureStageCount*sizeof(uint64_t) && bytes.size() <= 4096 ? &constant_banks[3] : nullptr;
  const size_t guest_bank = bytes.size() == 0x1000 ? 0 : 1;
  const bool tracked = swap && bank && track_guest_constants && !guest_constant_tracking_failed;
  if (tracked && bank->upload && bank->size == bytes.size() && guest_constants.Clean(guest_bank)) {
    bool matches = true;
    if (audit_guest_constants) {
      ++guest_constant_audits;
      matches = !std::memcmp(bank->source_view, bytes.data(), bytes.size());
    }
    if (matches) { ++guest_constant_skips; return bank->upload; }
    // A producer that bypasses the dirty contract must remain correct. The
    // diagnostic audit refreshes this draw and disables tracking for the run.
    guest_constant_tracking_failed = true;
    ++guest_constant_mismatches;
    REXLOG_ERROR("gta4-metal-constant-audit: unreported change bank={} draw={}; using exact snapshots", guest_bank, draws);
  }
  if (bank && bank->upload && bank->size == bytes.size() &&
      !std::memcmp(bank->source_view, bytes.data(), bytes.size())) {++constant_last_hits; if (tracked) guest_constants.Copied(guest_bank); return bank->upload;}
  const auto remember = [&](ui::metal::UploadSlice upload,const uint8_t* source) {
    if (bank && upload) {
      if(!source){std::memcpy(bank->source.data(),bytes.data(),bytes.size());source=bank->source.data();}
      bank->source_view=source;
      bank->size = bytes.size(); bank->upload = upload;
      if (tracked) guest_constants.Copied(guest_bank);
    }
    return upload;
  };
  const uint64_t hash = XXH3_64bits_withSeed(bytes.data(), bytes.size(), swap ? 1 : 0);
  if (const auto* cached = constants.Find(hash, bytes, swap)) {
    ++constant_hash_hits; return remember(*cached,constants.source_of_last_hit());
  }
  auto upload = frame->uploads.Allocate(context->device, bytes.size(), 16);
  if (!upload) { error = "Metal frame upload budget exceeded"; return {}; }
  if (swap) gta4_native::core::CopyGuestWordsToHost(static_cast<uint8_t*>(upload.data), bytes.data(), bytes.size());
  else std::memcpy(upload.data, bytes.data(), bytes.size());
  constant_upload_bytes += bytes.size();
  const auto* immutable_source=constants.Insert(hash,bytes,swap,upload,cache_constant_sources);
  return remember(upload,immutable_source);
}

bool Renderer::Submit(std::span<const std::byte> bytes,std::string& error,
    const gta4_native::FireTraceContext* trace) {
  ++state_->diagnostic_command;
  state_->diagnostic_point = "submit";
  state_->diagnostic_texture_stage = UINT32_MAX;
  state_->diagnostic_texture_handle = 0;
  state_->fire_context = trace ? *trace : gta4_native::FireTraceContext{};
  const bool timed = !state_->frame_log_path.empty();
  const auto submit_begin = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  const bool ok = SubmitImpl(bytes, error);
  if (timed) state_->frame_submit_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - submit_begin).count());
  if (!ok) state_->TraceCommandFailure(bytes,error);
  if (state_->sky_visibility) { state_->EndRender(); state_->sky_visibility=nil; }
  if (!ok && state_->fire_active) gta4_native::FireTraceLog("metal-command-error",
      fmt::format("capture={} draw={} event={} reason={}", state_->fire_capture,
          state_->fire_draw, state_->fire_context.event, error));
  state_->fire_context = {};
  if (!ok) state_->frame_qos.End();
  return ok;
}
void Renderer::State::TraceCommandFailure(std::span<const std::byte> bytes, std::string_view error) {
  if (!modern_diagnostics.enabled()) return;
  const auto header=bytes.size()>=sizeof(gta4_native::CommandHeader)
      ? Command<gta4_native::CommandHeader>(bytes) : gta4_native::CommandHeader{};
  const auto vs=shaders.find(vertex_shader), ps=shaders.find(pixel_shader);
  if (!modern_diagnostics.Failure(diagnostic_point,diagnostic_command,uint32_t(header.type),
      uint32_t(Phase()),vs==shaders.end()?0:vs->second.hash,ps==shaders.end()?0:ps->second.hash,error)) return;
  REXLOG_ERROR("gta4-modern-exec: backend=metal event=failure-state trace-frame={} command={} submission={} "
      "device={:08X} vs-handle={:08X} ps-handle={:08X} declaration={:08X} index={:08X} "
      "texture-stage={} texture={:08X} environment-seq={} valid={:016X} guest-event={}",
      modern_diagnostics.frame(),diagnostic_command,submitted,device,vertex_shader,pixel_shader,declaration,index_buffer,
      diagnostic_texture_stage,diagnostic_texture_handle,environment.source_sequence,environment.valid_fields,fire_context.event);
  for(size_t i=0;i<color_bindings.size();++i) {
    const auto& c=color_bindings[i];
    const auto surface=resources.FindSurface(c.handle);
    REXLOG_ERROR("gta4-modern-exec: backend=metal event=failure-target command={} slot={} handle={:08X} "
        "address={:08X} extent={}x{} sample-type={} format={:08X} flags={:08X} generation={} "
        "initialized={} produced={:X} serial={}",diagnostic_command,i,c.handle,c.address,c.width,c.height,
        c.sample_type,c.format,c.flags,surface?surface->generation:0,surface&&surface->initialized,
        surface?surface->content_mask:0,surface?surface->content_serial:0);
  }
  if(header.type==gta4_native::CommandType::kResolve && bytes.size()==sizeof(gta4_native::ResolveCommand)) {
    const auto c=Command<gta4_native::ResolveCommand>(bytes);
    REXLOG_ERROR("gta4-modern-exec: backend=metal event=failure-resolve command={} source={:08X} "
        "flags={:08X} destination={:08X} level={} slice={}",diagnostic_command,c.source.handle,c.flags,
        c.destination_texture,c.destination_level,c.destination_slice_or_face);
  }
}
bool Renderer::SubmitImpl(std::span<const std::byte> bytes,std::string& error) {
  @autoreleasepool {
    using namespace gta4_native;
    error.clear(); auto& s=*state_;
    if(!s.ready || bytes.size()<sizeof(CommandHeader)) {error="Renderer unavailable or short command"; return false;}
    const auto header=Command<CommandHeader>(bytes);
    if(!TitleCommandSize(header.type) || header.size!=bytes.size() || header.size!=TitleCommandSize(header.type)) {
      error="Invalid title command size/type"; return false;
    }
    if ((header.type == CommandType::kResourceUnlock || header.type == CommandType::kReleaseResource ||
         header.type == CommandType::kRegisterVirtualResource || header.type == CommandType::kRegisterReflectionTarget) &&
        !s.MaterializePendingClears(error)) return false;
    const auto observe_draw = [&]<class Draw>() {
      const auto c = Command<Draw>(bytes);
      s.guest_constants.Observe(c.device, c.dirty_state.words[0], c.dirty_state.words[1],
          RequiresFullLightingConstants(c.lighting));
    };
    switch (header.type) {
      case CommandType::kDrawPrimitive: observe_draw.template operator()<DrawPrimitiveCommand>(); break;
      case CommandType::kDrawPrimitiveUp: observe_draw.template operator()<DrawPrimitiveUpCommand>(); break;
      case CommandType::kDrawIndexedPrimitive: observe_draw.template operator()<DrawIndexedPrimitiveCommand>(); break;
      case CommandType::kClear: {
        const auto c = Command<ClearCommand>(bytes);
        s.guest_constants.Observe(c.device, c.dirty_state.words[0], c.dirty_state.words[1], false);
        break;
      }
      default: break;
    }
    switch(header.type) {
      case CommandType::kTemporalUpdate: {
        if(!s.temporal_enabled)return true;
        const auto c=Command<TemporalCommand>(bytes);auto& t=s.temporal_scene;
        if(c.event==TemporalEvent::kInstance){
          const bool current=c.sequence==t.metadata.sequence&&c.epoch==t.metadata.epoch;
          t.main_view=(c.flags&kTemporalMainView)&&current;
          t.execution_scene_geometry=(c.flags&kTemporalSceneGeometry)&&current;
          t.execution_screen_space=(c.flags&kTemporalScreenSpace)&&current;
          t.execution_attributed=(c.flags&kTemporalExecutionAttributed)&&current;
          t.execution_composite=(c.flags&kTemporalCompositeExecution)&&current;
          t.execution_final_composite=(c.flags&kTemporalFinalCompositeExecution)&&t.execution_composite;
          t.execution_composite_scope=t.execution_composite?c.time_ns:0;
          t.observed_view=c.view;t.observed_sequence=c.sequence;
          t.execution_camera_valid=(c.flags&kTemporalExecutionCamera)&&current;
          t.execution_projection=c.projection;t.execution_inverse=c.view_inverse;
          t.instance=c.instance;t.drawable=c.drawable;t.pose=c.pose;return true;
        }
        if(!s.Begin(error))return false;
        s.EndRender();
        if(c.event==TemporalEvent::kBeginScene){
          if(c.sequence%60==0)REXLOG_INFO("gta4-temporal-begin sequence={} epoch={} view={:X} previous-sequence={} previous-view={:X} previous-world={} phase={}",c.sequence,c.epoch,c.view,t.metadata.sequence,t.metadata.view,t.world_draws,uint32_t(s.Phase()));
          const auto method=s.anti_aliasing==AntiAliasingMode::kTaa?temporal::Method::kTaa:temporal::Method::kMetalFx;
          return t.Begin(s.commands,c,method,s.temporal_upscale?c.output_width:c.width,s.temporal_upscale?c.output_height:c.height,
              s.temporal_generation,error);
        }
        const bool current=c.sequence==t.metadata.sequence&&c.epoch==t.metadata.epoch&&t.active;
        const bool executed=current&&(c.flags&kTemporalCompositeExecution)&&c.time_ns;
        if(c.event==TemporalEvent::kBeforePostFx){
          if(!executed)return true;
          t.active_composite_scope=c.time_ns;t.executed_composite_source.reset();
          return t.CaptureDepth(s.commands,error);
        }
        if(c.event==TemporalEvent::kAfterPostFx){
          if(!executed||t.active_composite_scope!=c.time_ns)return true;
          if(!t.HasSceneOutput()&&t.executed_composite_source&&
              !t.RecoverComposite(s.commands,t.executed_composite_source,error))return false;
          t.active_composite_scope=0;
          return t.StartUi(s.commands,error);
        }
        if(c.event==TemporalEvent::kEndFrame){if(current)t.End();return true;}
        error="unknown temporal event";return false;
      }
      case CommandType::kDeviceCreated: {
        const auto c=Command<DeviceCommand>(bytes); s.device=c.device;
        s.guest_constants.Reset();
        if(c.mode!=2) {s.vertex_shader=0; s.pixel_shader=0; s.declaration=0; s.index_buffer=0; s.streams={};}
        return true;
      }
      case CommandType::kDeviceDestroyed:
        s.pipeline_lookup.Reset();
        s.guest_constants.Reset();
        if(!s.Flush(true,error)) return false;
        s.temporal_scene.Reset();
        s.resources.Clear(); s.shaders.clear(); s.declarations.clear(); s.modern.Reset(); s.phases.Reset();
        s.postfx_half_scene_handle = 0; s.depth_of_field.ReleaseExtentResources(); s.sun_shafts.ReleaseExtentResources();
        s.fusion_cloud_mask = nil; s.fusion_cloud_ready = false; return true;
      case CommandType::kRegisterShader: {
        const auto c=Command<RegisterShaderCommand>(bytes);
        if(!c.shader || !c.hash || (c.stage!=ShaderStage::kVertex && c.stage!=ShaderStage::kPixel)) {error="Invalid shader registration"; return false;}
        const auto existing = s.shaders.find(c.shader);
        if (existing != s.shaders.end() && existing->second.hash == c.hash && existing->second.stage == c.stage)
          return true;
        auto metadata=s.stock.Lookup(c.hash,c.stage,error);
        if(!metadata) return false;
        s.shaders.insert_or_assign(c.shader,std::move(*metadata)); return true;
      }
      case CommandType::kRegisterVertexDeclaration: {
        const auto c=Command<RegisterVertexDeclarationCommand>(bytes);
        if(!c.declaration || !c.element_count || c.element_count>kMaximumVertexElementCount || c.maximum_stream>=kVertexStreamCount) {
          error="Invalid vertex declaration"; return false;
        }
        // Registration is also emitted when a title binds an existing layout.
        // An unchanged layout must keep its identity, pipeline and converted bytes.
        for (uint32_t i = 0; i < c.element_count; ++i) {
          if (c.elements[i].stream >= kVertexStreamCount ||
              c.elements[i].stream > c.maximum_stream || c.elements[i].usage > 13) {
            error = "Invalid vertex declaration element"; return false;
          }
        }
        const auto existing = s.declarations.find(c.declaration);
        const auto same_element = [](const VertexElement& a, const VertexElement& b) {
          return a.stream == b.stream && a.offset == b.offset && a.type == b.type &&
                 a.method == b.method && a.usage == b.usage && a.usage_index == b.usage_index;
        };
        if (existing != s.declarations.end() && existing->second.elements.size() == c.element_count &&
            std::equal(existing->second.elements.begin(), existing->second.elements.end(), c.elements, same_element)) {
          ++s.declaration_reuses;
          return true;
        }
        if (s.next_declaration == UINT64_MAX) { error = "Vertex declaration identity exhausted"; return false; }
        VertexDeclaration value;
        value.identity = s.next_declaration++;
        value.elements.assign(c.elements, c.elements + c.element_count);
        s.declarations.insert_or_assign(c.declaration, std::move(value));
        ++s.declaration_definitions;
        return true;
      }
      case CommandType::kSetPixelShader:s.pixel_shader=Command<SetShaderCommand>(bytes).shader; return true;
      case CommandType::kSetVertexShader:s.vertex_shader=Command<SetShaderCommand>(bytes).shader; return true;
      case CommandType::kSetVertexDeclaration:s.declaration=Command<SetVertexDeclarationCommand>(bytes).declaration; return true;
      case CommandType::kSetIndexBuffer:s.index_buffer=Command<SetIndexBufferCommand>(bytes).buffer; return true;
      case CommandType::kSetVertexStream: {
        const auto c=Command<SetVertexStreamCommand>(bytes);
        if(c.stream>=kVertexStreamCount) {error="Vertex stream index out of range"; return false;}
        s.streams[c.stream]={c.buffer,c.offset,c.stride}; return true;
      }
      case CommandType::kSetRenderTarget: {
        const auto c=Command<SetRenderTargetCommand>(bytes);
        if(c.index>=kRenderTargetCount) {error="Color attachment index out of range"; return false;}
        s.color_bindings[c.index]=c.surface; return true;
      }
      case CommandType::kSetDepthStencil:s.depth_binding=Command<SetDepthStencilCommand>(bytes).surface; return true;
      case CommandType::kSetTexture: {
        const auto c=Command<SetTextureCommand>(bytes);
        if(c.stage>=kTextureStageCount || c.vector_font_id>3) {error="Texture stage/font identity out of range"; return false;}
        s.resources.RegisterFont(c.texture, c.vector_font_id);
        if (c.texture && rex::cvar::Query<bool>("gta4_metal_prepare_textures")) {
          const auto guest = s.memory.Read(c.device, kGuestDeviceSize);
          if (!guest.empty() && GuestWord(guest, 0x30F8 + c.stage * sizeof(uint32_t)) == c.texture) {
            xenos::xe_gpu_texture_fetch_t fetch{};
            auto* words = reinterpret_cast<uint32_t*>(&fetch);
            for (size_t word = 0; word < 6; ++word)
              words[word] = GuestWord(guest, 0x480 + c.stage * 0x18 + word * sizeof(uint32_t));
            s.resources.PrefetchTexture(c.texture, fetch);
          }
        }
        return true; // Draw validates the current image identity before consuming preparation.
      }
      case CommandType::kSetRenderState:return true; // Authoritative packed fields are read at Draw/Clear.
      case CommandType::kResourceUnlock: {
        const auto c=Command<ResourceUnlockCommand>(bytes);
        if(!c.resource || (c.access!=ResourceUnlockAccess::kNoDirtyUpdate && c.access!=ResourceUnlockAccess::kGuestWrite)) {
          error="Invalid resource unlock"; return false;
        }
        if(c.access==ResourceUnlockAccess::kGuestWrite) s.resources.Invalidate(c.resource);
        return true;
      }
      case CommandType::kReleaseResource: {
        const auto c=Command<ReleaseResourceCommand>(bytes);
        s.resources.Release(c.resource); s.shaders.erase(c.resource); s.declarations.erase(c.resource); return true;
      }
      case CommandType::kRegisterVirtualResource:
        if(!s.resources.RegisterVirtual(Command<RegisterVirtualResourceCommand>(bytes))) {error="Invalid virtual resource registration"; return false;}
        return true;
      case CommandType::kRegisterReflectionTarget: {
        const auto c=Command<RegisterReflectionTargetCommand>(bytes);
        if(!c.surface || !c.logical_width || !c.logical_height || !c.physical_width || !c.physical_height ||
            c.physical_width>16384 || c.physical_height>16384 || c.family>ReflectionFamily::kEnvironment || c.role>ReflectionRole::kDepth ||
            (c.sample_count_override && c.sample_count_override!=1 && c.sample_count_override!=2 && c.sample_count_override!=4)) {
          error="Invalid reflection registration"; return false;
        }
        s.resources.RegisterReflection(c); return true;
      }
      case CommandType::kUpdateEnvironmentalData: {
        const auto c=Command<UpdateEnvironmentalDataCommand>(bytes);
        if(c.reserved || !ValidEnvironmentalData(c.data)) {error="Invalid environmental snapshot"; return false;}
        s.environment=c.data; return true;
      }
      case CommandType::kRenderPhaseMarker: {
        const auto c=Command<RenderPhaseMarkerCommand>(bytes);
        const auto transition=s.phases.Apply(c);
        if(transition==RenderPhaseStack::Result::kInvalidPhase) {error="Invalid render phase"; return false;}
        if(transition==RenderPhaseStack::Result::kInvalidEvent) {error="Invalid render phase event"; return false;}
        s.postfx_half_scene_handle=s.phases.half_scene_texture();
        if(transition==RenderPhaseStack::Result::kOverflow) {error="Render phase nesting exceeds bound"; return false;}
        s.FirePhase(c);
        // This command carries attribution, not a resource dependency. Actual
        // target changes, clears, resolves, uploads and readbacks end encoders
        // at their own boundaries. Keep compatible draws in the existing pass.
        // An unmatched end recovers to Unknown, as in the Vulkan renderer.
        if (s.render && rex::diagnostics::IsEnabled(rex::diagnostics::Category::kNativeTrace))
          [s.render insertDebugSignpost:[NSString stringWithFormat:@"Title phase %u event %u", uint32_t(c.phase), uint32_t(c.event)]];
        return true;
      }
      case CommandType::kDrawPrimitive:case CommandType::kDrawPrimitiveUp:case CommandType::kDrawIndexedPrimitive:
        return s.Draw(header,bytes,error);
      case CommandType::kClear:return s.Clear(Command<ClearCommand>(bytes),error);
      case CommandType::kResolve: {
        const auto c=Command<ResolveCommand>(bytes);
        if(s.temporal_scene.active&&s.temporal_scene.depth_source&&c.source.handle==s.temporal_scene.depth_source->descriptor.handle){
          s.EndRender();if(!s.temporal_scene.CaptureDepth(s.commands,error))return false;
        }
        return s.Resolve(c,error);
      }
      case CommandType::kDepthSurfaceHandoff:return s.Handoff(Command<DepthSurfaceHandoffCommand>(bytes),error);
      case CommandType::kPresent:return s.Present(Command<PresentCommand>(bytes),error);
      default:error="Command requires the synchronous title interface"; return false;
    }
  }
}

bool Renderer::Execute(std::span<const std::byte> bytes,std::span<std::byte> output,std::string& error) {
  using namespace gta4_native;
  error.clear();
  if(!state_->ready || bytes.size()<sizeof(CommandHeader)) {error="Renderer unavailable or short command"; return false;}
  const auto header=Command<CommandHeader>(bytes);
  if(header.size!=bytes.size() || header.size!=TitleCommandSize(header.type)) {error="Invalid synchronous command size"; return false;}
  if(header.type==CommandType::kQueryDeviceCapabilities && output.size()==sizeof(DeviceCapabilitiesResult)) {
    // AA writes normalized perceptual FP16; the presenter upscales it before
    // EDR conversion. This capability describes the active backend, not a
    // pending renderer selection in the frontend.
    const auto caps=temporal::QueryCapabilities(state_->context->device);
    const DeviceCapabilitiesResult result{16384,kCapabilityPerceptualPresentationBeforeHdr|kCapabilityTemporalInputs|
        kCapabilityNativeTemporalAA|
        (caps.temporal?kCapabilityMetalFxTemporalUpscaling:0u)|
        (caps.interpolation?kCapabilityMetalFxFrameGeneration:0u)}; std::memcpy(output.data(),&result,sizeof(result)); return true;
  }
  if(header.type==CommandType::kTextureLock && output.size()==sizeof(TextureLockResult)) {
    TextureLockResult result{};
    if(!state_->Readback(Command<TextureLockCommand>(bytes),result,error)) return false;
    std::memcpy(output.data(),&result,sizeof(result)); return true;
  }
  error="Invalid synchronous command/result contract"; return false;
}
Renderer::Statistics Renderer::statistics() const {
  return {state_->pipeline_creations, state_->declaration_definitions,
          state_->declaration_reuses, state_->draws, state_->pipelines.size(), state_->render_passes_created,
          state_->clear_load_folds, state_->clear_materializations,
          state_->resolve_skips, state_->resolve_initializations_merged,
          state_->guest_constant_skips, state_->guest_constant_audits, state_->guest_constant_mismatches,
          state_->bindings.calls, state_->bindings.skipped,
          state_->bindings.residency_calls, state_->bindings.residency_skipped,
          state_->pipeline_lookups, state_->pipeline_lookup_hits,state_->skipped_pixel_constant_banks,
          state_->resources.material_statistics().hits,state_->resources.material_statistics().misses,state_->resolve_image_exchanges};
}
bool Renderer::Finish(std::string& error) {return state_->Flush(true,error);}

bool Renderer::OpenPipelineStore(const std::filesystem::path& cache_root,uint32_t title_id,std::string& error) {
  auto& s=*state_;
  if(!s.ready){error="Renderer is not initialized";return false;}
  s.use_pipeline_archive=rex::cvar::Query<bool>("gta4_metal_pipeline_archive");
  if(!s.use_pipeline_archive) return true;
  const uint64_t parts[4]={s.stock.Identity(),s.overrides.Identity(),s.temporal_stock.Identity(),s.temporal_overrides.Identity()};
  return s.pipeline_store.Open(s.context->device,cache_root,title_id,XXH3_64bits(parts,sizeof(parts)),error);
}

size_t Renderer::PendingPipelineCount() const {
  if(!state_->pipeline_store.is_open()) return 0;
  auto pending=state_->pipeline_store.Pending();
  return size_t(std::count_if(pending.begin(),pending.end(),[&](const auto& r){return state_->RecipeBuildable(r);}));
}

bool Renderer::PrecompilePipelines(const std::function<bool(uint32_t,uint32_t)>& progress,std::string& error) {
  auto& s=*state_;
  if(!s.pipeline_store.is_open()) return true;
  // Anything new means a full rebuild into a fresh archive (see PipelineStore::Reset).
  if(s.pipeline_store.Pending().empty()) return true;
  if(!s.pipeline_store.Reset(error)) return false;
  auto pending=s.pipeline_store.All();
  std::erase_if(pending,[&](const PipelineRecipe& r){return !s.RecipeBuildable(r);});
  // Depth-only (no fragment stage) pipelines cannot be serialized into a binary archive: one of
  // them makes the whole archive fail to save ("expecting 'fragment' stage"). They compile quickly
  // at runtime, so mark them handled instead of archiving them.
  std::erase_if(pending,[&](const PipelineRecipe& r){
    if(r.fragment.library!=RecipeLibrary::kNone) return false;
    s.pipeline_store.Skip(HashRecipe(r));
    return true;
  });
  // Group by shader so the bounded library/function caches are reused rather than thrashed.
  std::sort(pending.begin(),pending.end(),[](const PipelineRecipe& a,const PipelineRecipe& b){
    return std::tie(a.vertex.hash,a.fragment.hash)<std::tie(b.vertex.hash,b.fragment.hash);
  });
  const uint32_t total=uint32_t(pending.size());
  std::atomic<uint32_t> done{0},compiled{0},skipped{0};
  bool keep_going=!progress||progress(0,total);
  // Descriptors are resolved serially (the shader caches are single-owner), then each batch is
  // compiled into the binary archive concurrently: Metal's compiler service runs them in parallel.
  constexpr size_t kBatch=64;
  // The archive serializer re-reads every added function's AIR at Save(). The shader caches evict
  // functions (LRU, 512), and an evicted specialized function frees that AIR, so Save() faulted in
  // a loop (rex's signal handler resumes it) and the loading screen never finished. Keep every
  // descriptor - and with it every function - alive until the archive is saved.
  std::vector<MTLRenderPipelineDescriptor*> keep_alive;
  keep_alive.reserve(pending.size());
  for(size_t begin=0;begin<pending.size()&&keep_going;begin+=kBatch){
    @autoreleasepool {
      const size_t end=std::min(pending.size(),begin+kBatch);
      std::vector<MTLRenderPipelineDescriptor*> descriptors(end-begin,nil);
      for(size_t i=begin;i<end;++i){
        const auto& recipe=pending[i];
        std::string item_error;
        id<MTLFunction> vertex=s.RecipeFunctionObject(recipe.vertex,item_error);
        id<MTLFunction> fragment=vertex&&recipe.fragment.library!=RecipeLibrary::kNone?
            s.RecipeFunctionObject(recipe.fragment,item_error):nil;
        if(vertex&&(fragment||recipe.fragment.library==RecipeLibrary::kNone))
          descriptors[i-begin]=DescriptorFromRecipe(recipe,vertex,fragment);
        else REXLOG_DEBUG("gta4-metal-pipeline-cache: skipped recipe vs={:016X} ps={:016X}: {}",
                          recipe.vertex.hash,recipe.fragment.hash,item_error);
      }
      for(auto* descriptor:descriptors) if(descriptor) keep_alive.push_back(descriptor);
      // Blocks copy captured C++ objects; capture plain pointers instead.
      auto* store=&s.pipeline_store; const PipelineRecipe* recipes=pending.data()+begin;
      MTLRenderPipelineDescriptor* __strong* batch=descriptors.data();
      auto* compiled_count=&compiled; auto* skipped_count=&skipped; auto* done_count=&done;
      dispatch_apply(descriptors.size(),dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^(size_t i){
        const auto& recipe=recipes[i];
        std::string item_error;
        if(batch[i]&&store->Add(batch[i],HashRecipe(recipe),item_error)) ++*compiled_count;
        else {
          // A shader removed by a newer build, or a descriptor Metal rejects: not retried every launch.
          ++*skipped_count;
          store->Skip(HashRecipe(recipe));
          if(batch[i]) REXLOG_DEBUG("gta4-metal-pipeline-cache: archive add failed vs={:016X} ps={:016X}: {}",
                                    recipe.vertex.hash,recipe.fragment.hash,item_error);
        }
        ++*done_count;
      });
    }
    if(progress) keep_going=progress(done.load(),total);
  }
  REXLOG_INFO("gta4-metal-pipeline-cache: precompiled {} pipelines ({} skipped)",compiled.load(),skipped.load());
  const auto save_begin=std::chrono::steady_clock::now();
  const bool saved=s.pipeline_store.Save(error);
  REXLOG_INFO("gta4-metal-pipeline-cache: archive save {} in {} ms",saved?"ok":"FAILED",
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-save_begin).count());
  return saved;
}
void Renderer::State::RecordFrameSample(uint32_t frame) {
  const uint64_t now=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  if(!frame_log_origin_ns) frame_log_origin_ns=now;
  const auto prep=resources.preparation_statistics();
  if(frame_samples.size()<frame_samples.capacity()){
    frame_samples.push_back({frame,now-frame_log_origin_ns,frame_last_present_ns?now-frame_last_present_ns:0,
        frame_submit_ns,frame_present_ns,uint32_t(draws-frame_prev_draws),uint32_t(pipeline_creations-frame_prev_pipelines),
        uint32_t(pipeline_waits-frame_prev_waits),uint32_t(prep.waited-frame_prev_texture_waits),pipeline_wait_ns-frame_prev_wait_ns,
        frame_gpu_ns->load()-frame_prev_gpu_ns});
  }
  frame_last_present_ns=now; frame_submit_ns=0; frame_present_ns=0;
  // Hard process exit can skip destructors: flush in rare, small batches instead of only at shutdown.
  if(frame_samples.size()>=600) WriteFrameLog();
  frame_prev_draws=draws; frame_prev_pipelines=pipeline_creations; frame_prev_waits=pipeline_waits;
  frame_prev_wait_ns=pipeline_wait_ns; frame_prev_texture_waits=prep.waited; frame_prev_gpu_ns=frame_gpu_ns->load();
}

void Renderer::State::WriteFrameLog() {
  if(frame_log_path.empty()||frame_samples.empty()) return;
  static bool header_written=false;
  std::ofstream out(frame_log_path,header_written?std::ios::app:std::ios::trunc);
  if(!header_written) out<<"frame,at_ms,interval_ms,submit_ms,present_ms,draws,pipelines_built,pipeline_waits,pipeline_wait_ms,texture_waits,gpu_ms\n";
  header_written=true;
  for(const auto& f:frame_samples)
    out<<f.frame<<','<<f.at_ns/1e6<<','<<f.interval_ns/1e6<<','<<f.submit_ns/1e6<<','<<f.present_ns/1e6<<','
       <<f.draws<<','<<f.pipelines_built<<','<<f.pipeline_waits<<','<<f.pipeline_wait_ns/1e6<<','<<f.texture_waits<<','<<f.gpu_ns/1e6<<'\n';
  frame_samples.clear();
}

void Renderer::State::FinishScopeProfile() {
  if(!scope_profile.active()) return;
  REXLOG_INFO("gta4-metal-profile-scope recording={} scope={} width={} height={} entries={} draws={} overflow={}",
      scope_profile.recording(),scope_profile.scope(),scope_profile.width(),scope_profile.height(),
      scope_profile.members().size(),scope_profile.draws(),scope_profile.overflow());
  for(const auto& value:scope_profile.members()) REXLOG_INFO(
      "gta4-metal-profile-member recording={} scope={} phase={} vs={:016X} ps={:016X} variant={:X} samples={} draws={} vertices={} indices={}",
      scope_profile.recording(),scope_profile.scope(),value.key.phase,value.key.vertex,value.key.pixel,
      value.key.variant,value.key.samples,value.draws,value.vertices,value.indices);
  scope_profile.End();
}
void Renderer::State::ProfileRead(const std::shared_ptr<TextureResource>& texture,uint32_t kind) {
  if(!profile_enabled || !texture || !texture->gpu_produced) return;
  for(const auto& [subresource,writer]:texture->subresource_writes)
    resolve_profile.Read(texture->generation,uint32_t(subresource>>32),uint32_t(subresource),writer,kind);
}
void Renderer::State::FinishResolveProfile() {
  if(!profile_enabled || !commands) return;
  for(const auto& value:resolve_profile.records()) {
    const auto& key=value.key;
    REXLOG_INFO("gta4-metal-profile-resolve recording={} seq={} source={:08X} source-generation={} source-writer={} destination={:08X} destination-generation={} destination-writer={} level={} slice={} source-format={} destination-format={} direct={} reused={} clear-flags={:X} first-bound-read={} read-kind={} source-origin={},{} destination-origin={},{} source-extent={},{} destination-extent={},{} sample-content={} sample-requested={} sample-physical={} sample-select={} convert-flags={:X}",
        key.recording,value.sequence,value.source,key.source_generation,key.source_writer,value.destination,
        key.destination_generation,value.destination_writer,key.level,key.slice,key.source_format,key.destination_format,
        key.direct,value.reused,value.clear_flags,value.first_read,value.read_kind,
        int32_t(key.conversion[0]),int32_t(key.conversion[1]),int32_t(key.conversion[2]),int32_t(key.conversion[3]),
        key.conversion[12],key.conversion[13],key.conversion[14],key.conversion[15],
        key.conversion[4],key.conversion[5],key.conversion[9],key.conversion[7],key.conversion[11]);
  }
  if(resolve_profile.overflow()) REXLOG_INFO("gta4-metal-profile-overflow recording={} resolves={}",submitted+1,resolve_profile.overflow());
}
#include "fire_trace.inc"
}  // namespace rex::graphics::gta4_metal
