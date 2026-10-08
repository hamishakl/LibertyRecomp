#include "renderer_state.h"
#include "../../ui/metal/guest_output_context.h"
#include <rex/cvar.h>
#include <rex/diagnostics/policy.h>
#include <rex/logging.h>

#include <bit>

namespace rex::graphics::gta4_metal {

bool Renderer::State::Present(const gta4_native::PresentCommand& present, std::string& error) {
  if (!present.frontbuffer_texture || !present.width || !present.height ||
      present.width > 16384 || present.height > 16384) {
    error = "Invalid title frontbuffer publication"; return false;
  }
  if (!Begin(error) || !MaterializePendingClears(error)) return false;
  auto source = PrepareTexture(present.frontbuffer_texture,
      std::bit_cast<xenos::xe_gpu_texture_fetch_t>(present.frontbuffer_fetch), error);
  if (!source || !source->initialized || source->image.textureType != MTLTextureType2D ||
      source->image.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
    if (error.empty()) error = "Title frontbuffer is not a ready color image"; return false;
  }
  ProfileRead(source,2);
  FireSnapshot(source->image,"present-before-aa",present.frontbuffer_texture,false,true);
  // One protected command recording contains the scene AND its mailbox copy.
  // RefreshGuestOutput invokes its writer before publishing or waiting for
  // admission. Commit there, so two slots cover two complete game frames rather
  // than a scene/copy pair. Readbacks keep their separate completion boundaries.
  EndRender();
  commands.label = [NSString stringWithFormat:@"Liberty game frame %u batch %llu",
      present.submitted_frame, submitted];
  const bool first_scene = !traced_scene && target_observations[size_t(gta4_native::RenderPhase::kSceneToGBuffer)].draws;
  const bool trace_extent = rex::diagnostics::IsEnabled(rex::diagnostics::Category::kNativeTrace) &&
      (first_scene || present.submitted_frame <= 3 || present.submitted_frame % 120 == 0);
  if (trace_extent) {
    const auto prep = resources.preparation_statistics();
    REXLOG_INFO("gta4-metal-preparation frame={} queued={} ready={} waits={} inline={} staging-allocations={} staging-reuses={} pending-bytes={} staging-bytes={} pipeline-ready={} pipeline-waits={}",
        present.submitted_frame, prep.queued, prep.ready, prep.waited, prep.inline_decodes,
        prep.staging_allocations, prep.staging_reuses, prep.pending_bytes, prep.staging_bytes,
        pipeline_ready, pipeline_waits);
    REXLOG_INFO("gta4-metal-cache frame={} pipelines-created={} pipeline-entries={} layout-definitions={} layout-reuses={} render-passes={}",
        present.submitted_frame, pipeline_creations, pipelines.size(), declaration_definitions, declaration_reuses, render_passes_created);
    REXLOG_INFO("gta4-metal-resolution point=title frame={} texture={:08X} generation={} source={}x{} logical={}x{} display={}x{} aa={} draws={}",
        present.submitted_frame, present.frontbuffer_texture, source->generation, source->image.width, source->image.height,
        present.width, present.height, present.display_width, present.display_height, uint32_t(anti_aliasing), frame_draws);
    for (size_t phase = 0; phase < target_observations.size(); ++phase) {
      const auto& item = target_observations[phase];
      if (!item.draws) continue;
      REXLOG_INFO("gta4-metal-resolution point=draw frame={} phase={} draws={} color={:08X} depth={:08X} logical={}x{} physical={}x{} depth-physical={}x{} samples={} viewport={},{},{},{} projection-scale={},{}",
          present.submitted_frame, phase, item.draws, item.color, item.depth, item.logical_width, item.logical_height,
          item.width, item.height, item.depth_width, item.depth_height, item.samples,
          item.viewport[0], item.viewport[1], item.viewport[2], item.viewport[3], environment.projection_matrix[0], environment.projection_matrix[5]);
    }
    for (size_t reason = 0; reason < pass_breaks.size(); ++reason)
      if (pass_breaks[reason]) REXLOG_INFO("gta4-metal-pass-break frame={} mask={} count={}",
          present.submitted_frame, reason, pass_breaks[reason]);
    const auto buffers = resources.buffer_cache_statistics();
    REXLOG_INFO("gta4-metal-buffer-cache frame={} captured={} entries={} evictions={}",
        present.submitted_frame, buffers.captured_bytes, buffers.entries, buffers.evictions);
    if (first_scene) traced_scene = true;
  }
  auto& temporal=temporal_scene;
  const bool scene_output=temporal.active&&temporal.HasSceneOutput()&&temporal.ui_active&&temporal.final_scene;
  const bool temporal_output=scene_output&&temporal.ui_exact;
  const bool generation_enabled=temporal_output&&temporal.resolved&&temporal.frame_generation&&rex::cvar::GetFlagByName("gta4_present_mode")!="immediate";
  temporal::InterpolationOutput generated_pair;
  if(generation_enabled){
    std::string generation_error;
    if (temporal.NeedsMotionCoverage()) {
      // Interpolation has no reactive mask: validate this frame's GPU-written
      // motion before admitting it to MetalFX history. AA-only and known
      // incomplete frames retain the normal asynchronous submission path.
      if (temporal.ValidateMotionCoverage(commands, generation_error)) {
        if (!Flush(true, error) || !Begin(error)) return false;
      } else {
        REXLOG_WARN("gta4-metal-frame-generation: frame={} coverage validation failed: {}",
                    present.submitted_frame, generation_error);
      }
    }
    if(!temporal.Generate(commands,generated_pair,generation_error))REXLOG_WARN("gta4-metal-frame-generation: frame={} skipped: {}",present.submitted_frame,generation_error);
    if(generated_pair.real&&!temporal.Compose(commands,temporal.final_scene,generated_pair.real,error))return false;
    if(generated_pair.generated&&!temporal.ComposeInPlace(commands,generated_pair.generated,error))return false;
  }
  // An unsupported affine HUD still exists in the original guest composite.
  // Preserve it through the normal final transfer at the requested extent;
  // scene jitter was removed before the composite and must not be applied here.
  const uint32_t output_width=scene_output?temporal.config.output_width:present.width;
  const uint32_t output_height=scene_output?temporal.config.output_height:present.height;
  bool published = true;
  if (presenter) {
    ui::GuestOutputProvenance provenance{};
    provenance.frame_rate_limit = rex::cvar::Query<uint32_t>("gta4_frame_limit");
    provenance.producer_backpressure = present.device != 0;
    provenance.paired_presentation=generated_pair.generated!=nil;
    if(provenance.paired_presentation&&provenance.frame_rate_limit)provenance.frame_rate_limit=std::min(1000u,provenance.frame_rate_limit*2);
    provenance.title_present_id = present.diagnostic_present_id;
    provenance.selected_generation = source->generation;
    provenance.submitted_frame = present.submitted_frame;
    provenance.selected_texture = present.frontbuffer_texture;
    provenance.present_origin = present.diagnostic_origin;
    provenance.guest_caller = present.diagnostic_guest_caller;
    provenance.diagnostic_trace = present.diagnostic_trace != 0;
    provenance.tv_session_id = present.diagnostic_tv_session_id;
    provenance.tv_final_sequence = present.diagnostic_tv_final_sequence;
    provenance.tv_movie_sequence = present.diagnostic_tv_movie_sequence;
    provenance.tv_rect_sequence = present.diagnostic_tv_rect_sequence;
    provenance.tv_bink_sequence = present.diagnostic_tv_bink_sequence;
    provenance.tv_bink_result = present.diagnostic_tv_bink_result;
    published = presenter->RefreshGuestOutput(output_width, output_height,
        present.display_width ? present.display_width : present.width,
        present.display_height ? present.display_height : present.height,
        [&](ui::Presenter::GuestOutputRefreshContext& output) {
          auto& metal = static_cast<ui::metal::MetalGuestOutputRefreshContext&>(output);
          if (trace_extent) REXLOG_INFO("gta4-metal-resolution point=mailbox frame={} source={}x{} destination={}x{}",
              present.submitted_frame, source->image.width, source->image.height, metal.texture().width, metal.texture().height);
          // The mailbox stores filtered FP16 values even for an 8-bit guest.
          output.SetIs8bpc(false);
          if (!Begin(error)) return false;
          EndRender();
          metal.BeginWrite(commands);
          // Added output noise belongs after resampling/sharpening. The
          // presenter selects the last-pass dither variant, not this mailbox.
          const auto quality = rex::cvar::GetFlagByName("gta4_native_smaa_quality");
          bool recorded=false;
          if(temporal_output){
            recorded=generated_pair.real?temporal.CopyColor(commands,generated_pair.real,metal.texture(),error):
                temporal.Compose(commands,temporal.final_scene,metal.texture(),error);
            if(recorded&&generated_pair.generated)metal.SetGeneratedFrame(generated_pair.generated,generated_pair.real,generated_pair.lease,temporal.metadata.epoch,temporal.presentation_interval_ns);
          }else recorded=post_processing.Record(commands,source->image,metal.texture(),anti_aliasing,quality,false,error);
          if (recorded) FireSnapshot(metal.texture(),"present-after-aa",0,false,true);
          if (recorded && trace_extent) {
            const auto route = gta4_native::GetAntiAliasingRoute(anti_aliasing);
            REXLOG_INFO("gta4-metal-aa frame={} mode={} scene-samples={} smaa={} quality={} source-samples={} destination-samples={}",
                present.submitted_frame, gta4_native::AntiAliasingModeName(anti_aliasing),
                route.scene_sample_count, route.presentation_smaa, quality,
                source->image.sampleCount, metal.texture().sampleCount);
          }
          if(recorded)metal.EndWrite(commands);
          return recorded && Flush(false, error);
        }, provenance);
  } else if (!Flush(false, error)) {
    return false;
  }
  // Match the title's completed-frame acknowledgement even if its surface was
  // hidden or disconnected. Never leave the guest waiting on an invisible UI.
  if (present.device && uint64_t(present.device) + 16552 + sizeof(uint32_t) <= 0x100000000ull) {
    const uint32_t completed = __builtin_bswap32(present.submitted_frame);
    if (!memory.Write(present.device + 16552,
        {reinterpret_cast<const uint8_t*>(&completed), sizeof(completed)})) {
      error = "Cannot acknowledge the completed title frame"; published = false;
    }
  }
  if(temporal.active&&(present.submitted_frame<=6||present.submitted_frame%60==0))
    REXLOG_INFO("gta4-metal-temporal-frame frame={} input={}x{} output={}x{} world={} history={} reactive={} ui={} resolved={} ui-exact={} generated={} geometry-bytes={} jittered={} composite={} composite-shader={:016X} scene-stage={} spatial-fallback={} fallback-reason={} generation-skip={}",
      present.submitted_frame,temporal.config.input_width,temporal.config.input_height,output_width,output_height,temporal.world_draws,temporal.history_draws,
      temporal.reactive_draws,temporal.ui_draws,temporal.resolved,temporal.ui_exact,generated_pair.generated!=nil,temporal.geometry.owned_bytes(),
      temporal.jittered_draws,temporal.composite_seen,temporal.composite_shader,temporal.composite_scene_stage,temporal.spatial_fallback,temporal.fallback_reason,temporal.generation_skip_reason);
  temporal.End();
  modern.EndGuestFrame();
  FireAdvance(present.submitted_frame);
  fusion_cloud_ready = false;
  if (present.submitted_frame <= 3 || present.submitted_frame % 300 == 0) {
    REXLOG_INFO("gta4-metal-constant-tracking frame={} skips={} audited={} mismatches={} fallback={}",
        present.submitted_frame, guest_constant_skips, guest_constant_audits,
        guest_constant_mismatches, guest_constant_tracking_failed);
    REXLOG_INFO("gta4-metal-pipeline-lookup frame={} lookups={} memo-hits={} compiled={} ready={} waited={} wait-ms={:.1f}",
        present.submitted_frame, pipeline_lookups, pipeline_lookup_hits,
        pipeline_creations, pipeline_ready, pipeline_waits, double(pipeline_wait_ns) / 1e6);
    const auto texture_cache = resources.texture_cache_statistics();
    const auto vertex_cache = resources.vertex_conversion_statistics();
    REXLOG_INFO("gta4-metal-vertex-cache frame={} bytes={} charged={} budget={} entries={} hits={} misses={} evictions={}",
        present.submitted_frame, vertex_cache.bytes, vertex_cache.charged_bytes,
        vertex_cache.budget, vertex_cache.entries, vertex_cache.hits,
        vertex_cache.misses, vertex_cache.evictions);
    REXLOG_INFO("gta4-metal: frame={} draws={} resolves={} clears={} published={} textures={} buffers={} uploads={} resolve-reuse={} resolve-init-merges={} clear-folds={} clear-materialized={} state-calls={} state-skips={} residency-calls={} residency-skips={} constant-last-hits={} constant-hash-hits={} constant-upload-bytes={} texture-budget={} texture-entries={} texture-evictions={} texture-rejections={}",
        present.submitted_frame, frame_draws, frame_resolves, frame_clears, published,
        resources.texture_bytes(), resources.buffer_bytes(), frames.reserved_bytes(),
        resolve_skips, resolve_initializations_merged,clear_load_folds,clear_materializations,
        bindings.calls,bindings.skipped,bindings.residency_calls,bindings.residency_skipped,
        constant_last_hits,constant_hash_hits,constant_upload_bytes,
        texture_cache.budget,texture_cache.entries,texture_cache.evictions,texture_cache.rejections);
  }
  frame_draws = frame_resolves = frame_clears = 0;
  target_observations = {}; pass_breaks = {};
  if (!published && error.empty()) error = "Presenter did not accept the title output";
  return published;
}
}  // namespace rex::graphics::gta4_metal
