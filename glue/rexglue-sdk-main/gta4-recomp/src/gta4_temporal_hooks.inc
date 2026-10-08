// Host-only temporal metadata. Original retail instruction bodies are untouched.
// The primary viewport is the same embedded grcViewport at renderContext+176
// used by generated sub_8267D890/sub_8267DAB8 (.46). Projection and inverse view
// offsets are shared with the verified environmental capture above.
namespace temporal_host {
struct Instance {
  uint64_t matrix = 0, drawable = 0, pose = 0;
};
thread_local Instance instance;
std::atomic<bool> enabled{false};
std::atomic<uint32_t> primary_view{0};
TemporalCommand scene{};
uint64_t epoch = 1;
std::mutex scene_mutex;
class Scope {
 public:
  Scope(uint64_t matrix, uint64_t drawable, uint64_t pose) : old_(instance) {
    instance = {matrix, drawable, pose};
  }
  ~Scope() { instance = old_; }

 private:
  Instance old_;
};
bool Requested() {
  const auto upscaler = rex::cvar::GetFlagByName("gta4_native_upscaler");
  return UsesTemporalAntiAliasing(GetActiveAntiAliasingMode()) ||
         upscaler == "metalfx" || upscaler == "fsr3" || upscaler == "dlss";
}
void Begin(uint8_t* base, uint32_t device, uint32_t viewport) {
  const bool requested=Requested();
  gta4::gpu_pass::temporal_inputs.store(requested,std::memory_order_relaxed);
  if (!device || !viewport || !requested)
    return;
  DeviceCapabilitiesResult caps{};
  if (!QueryNativeDeviceCapabilities(caps) || !(caps.capabilities & kCapabilityTemporalInputs))
    return;
  TemporalCommand next;
  next.event = TemporalEvent::kBeginScene;
  next.device = device;
  next.view = viewport;
  next.sequence = uint64_t(LoadU32(base, device + kSubmittedFrameOffset)) + 1;
  next.time_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
  if (!LoadFiniteGuestFloats(base, viewport + kViewportProjectionOffset, next.projection) ||
      !LoadFiniteGuestFloats(base, viewport + kViewportViewInverseOffset, next.view_inverse))
    return;
  const auto camera = rex::graphics::gta4_metal::temporal::DescribeProjection(next.projection);
  if (!camera)
    return;
  next.near_plane = camera->near_plane;
  next.far_plane = camera->far_plane;
  next.field_of_view = camera->field_of_view;
  next.aspect = camera->aspect;
  {
    std::lock_guard lock(g_native_resolution_mutex);
    next.width = g_native_resolution.render_width;
    next.height = g_native_resolution.render_height;
    next.output_width = g_native_resolution.display_width;
    next.output_height = g_native_resolution.display_height;
  }
  if (!next.width || !next.height)
    return;
  const auto boundary=viewport>=gta4::temporal_boundary::kViewportOffset?
      gta4::temporal_boundary::DeclaredBoundary(base,device,next.sequence,
          viewport-gta4::temporal_boundary::kViewportOffset):gta4::temporal_boundary::Decision{};
  const auto upscaler = rex::cvar::GetFlagByName("gta4_native_upscaler");
  if (upscaler == "fsr3" || upscaler == "dlss") {
    TemporalUpscalerResult provider;
    if (!QueryNativeTemporalUpscaler(next.output_width, next.output_height, next.sequence, provider) ||
        provider.status != TemporalUpscalerStatus::kReady ||
        provider.render_width != next.width || provider.render_height != next.height ||
        !std::isfinite(provider.jitter[0]) || !std::isfinite(provider.jitter[1])) {
      enabled.store(false, std::memory_order_release);
      return;
    }
    if (boundary) next.jitter = provider.jitter;
  } else if (boundary) {
    next.jitter = rex::graphics::gta4_metal::temporal::Jitter(next.sequence);
  }
  next.flags = (boundary?kTemporalJitterApplied:0) | (camera->reversed ? kTemporalDepthReversed : 0);
  {
    std::lock_guard lock(scene_mutex);
    // The scheduler may enter the same primary phase more than once while its
    // deferred scene executes. Keep that scene's jitter, time and history epoch
    // stable; the actual camera is adopted from its executed primary draws.
    if (SameTemporalScene(next,scene))
      return;
    if (scene.sequence &&
        (next.device != scene.device || next.sequence <= scene.sequence || next.view != scene.view))
      ++epoch;
    if (scene.sequence && rex::graphics::gta4_metal::temporal::CameraDiscontinuity(
                              scene.view_inverse, next.view_inverse))
      next.flags |= kTemporalCameraCut;
    next.epoch = epoch;
    scene = next;
  }
  primary_view.store(viewport, std::memory_order_release);
  enabled.store(true, std::memory_order_release);
  static std::atomic<uint64_t> boundary_observations{0};
  if(boundary_observations.fetch_add(1,std::memory_order_relaxed)<4||next.sequence%60==0)
    REXLOG_INFO("gta4-temporal-boundary frame={} view={:X} admitted={} composite-phase={:X} reason={}",
        next.sequence,next.view,bool(boundary),boundary.composite_phase,boundary.reason);
  SubmitNativeCommand(next);
}
void Event(TemporalEvent event) {
  if (!enabled.load(std::memory_order_acquire))
    return;
  TemporalCommand command;
  {
    std::lock_guard lock(scene_mutex);
    command = scene;
  }
  command.event = event;
  const auto lighting=GetNativeLightingContext();
  thread_local std::vector<TemporalCommand> postfx_scopes;
  const auto composite_flags=TemporalCompositeExecutionFlags(lighting,false);
  if((event==TemporalEvent::kBeforePostFx||event==TemporalEvent::kAfterPostFx)&&composite_flags){
    command.flags|=composite_flags;
    command.time_ns=lighting.occurrence_id;
    if(event==TemporalEvent::kBeforePostFx){
      postfx_scopes.push_back(command);
    }else{
      // Another scheduler thread may have published its next BeginScene while
      // this synchronous postfx call ran. Close the exact frame/scope begun on
      // this execution thread, never the newer global scheduling snapshot.
      if(postfx_scopes.empty()||postfx_scopes.back().time_ns!=lighting.occurrence_id)return;
      command=postfx_scopes.back();postfx_scopes.pop_back();
      command.event=event;
    }
  }
  SubmitNativeCommand(command);
}
void Draw(uint8_t* base, uint32_t device) {
  if (!enabled.load(std::memory_order_acquire) || !device)
    return;
  TemporalCommand c;
  c.event = TemporalEvent::kInstance;
  c.device = device;
  c.view = LoadU32(base, kCurrentViewportGlobal);
  c.sequence = uint64_t(LoadU32(base, device + kSubmittedFrameOffset)) + 1;
  bool classified_main = false, classified_camera_valid = false;
  std::array<float, 16> classified_projection{}, classified_inverse{};
  {
    TemporalCommand primary;
    {
      std::lock_guard lock(scene_mutex);
      primary = scene;
    }
    if (c.sequence == primary.sequence && c.device == primary.device) {
      c.epoch = primary.epoch;
      std::array<float, 16> projection{}, inverse{};
      if (c.view && LoadFiniteGuestFloats(base, c.view + kViewportProjectionOffset, projection) &&
          LoadFiniteGuestFloats(base, c.view + kViewportViewInverseOffset, inverse)) {
        const auto equal = [](const auto& a, const auto& b) {
          for (size_t i = 0; i < a.size(); ++i)
            if (std::abs(a[i] - b[i]) > 0.0001f * std::max({1.0f, std::abs(a[i]), std::abs(b[i])}))
              return false;
          return true;
        };
        // Retail queues copies of grcViewport. Their addresses identify command
        // storage, not different cameras. Compare the unchanged camera fields;
        // model-world/WVP regions deliberately do not participate.
        classified_main =
            equal(projection, primary.projection) && equal(inverse, primary.view_inverse);
        classified_projection = projection;
        classified_inverse = inverse;
        classified_camera_valid =
            rex::graphics::gta4_metal::temporal::DescribeProjection(projection).has_value();
        if (c.sequence % 60 == 0) {
          thread_local uint64_t logged_sequence = 0;
          thread_local uint32_t logged = 0;
          if (logged_sequence != c.sequence) {
            logged_sequence = c.sequence;
            logged = 0;
          }
          if (classified_main && logged++ < 2)
            REXLOG_INFO(
                "gta4-temporal-camera: frame={} source={:X} execution-copy={:X} camera-match=true",
                c.sequence, primary.view, c.view);
        }
      }
    }
  }
  c.flags = TemporalExecutionFlags(gta4::gpu_pass::current.attributed(),gta4::gpu_pass::current.retail_phase) |
            (classified_main ? kTemporalMainView : 0) |
            (classified_camera_valid ? kTemporalExecutionCamera : 0);
  const auto lighting=GetNativeLightingContext();
  const auto composite_flags=TemporalCompositeExecutionFlags(lighting,gta4::gpu_pass::final_composite);
  if(composite_flags){
    c.flags|=composite_flags;
    c.time_ns=lighting.occurrence_id;
  }
  if (classified_camera_valid) {
    c.projection = classified_projection;
    c.view_inverse = classified_inverse;
  }
  c.instance = instance.matrix;
  c.drawable = instance.drawable;
  c.pose = instance.pose;
  // A cached list is an execution, not a durable object. Scope comes from its
  // live model/pose owner, never from a draw ordinal or captured register value.
  thread_local TemporalCommand previous{};
  if (c.device == previous.device && c.view == previous.view && c.sequence == previous.sequence &&
      c.epoch == previous.epoch && c.time_ns == previous.time_ns &&
      c.projection == previous.projection && c.view_inverse == previous.view_inverse &&
      c.flags == previous.flags && c.instance == previous.instance &&
      c.drawable == previous.drawable && c.pose == previous.pose)
    return;
  if (SubmitNativeCommand(c))
    previous = c;
}
}  // namespace temporal_host
