// Opt-in observation of the compiled retail fade path. Every hook invokes the
// original generated implementation exactly once and retains its register result.
// Evidence: generated .17 sub_823469F8 / sub_82346900 / sub_82345FD8;
// generated .19 sub_82369AA8. Generated translation units are not modified.
namespace {
struct FadeObservedState {
  uint32_t alpha = UINT32_MAX;
  uint32_t dispatch = UINT32_MAX;
};
std::mutex g_fade_observation_mutex;
std::unordered_map<uint32_t, FadeObservedState> g_fade_observations;
std::atomic<uint64_t> g_fade_record_count{0};
thread_local uint32_t g_fade_builder_object = 0;
thread_local uint32_t g_fade_builder_kind = 0;  // 1=viewport effect, 2=main screen fade.
thread_local uint32_t g_screen_fade_emissions = 0;
struct ScreenFadeObservedState {
  uint32_t alpha = UINT32_MAX, mode = UINT32_MAX, active = UINT32_MAX;
  bool drawing = false;
};
std::mutex g_screen_fade_mutex;
std::unordered_map<uint32_t, ScreenFadeObservedState> g_screen_fade_states;
struct FadeDcOrigin {
  uint32_t object = 0, token = 0, color = 0, creator = 0, kind = 0;
  uint64_t run = 0, event = 0;
};
std::mutex g_fade_dc_mutex;
std::unordered_map<uint32_t, FadeDcOrigin> g_fade_dc_origins;
thread_local uint32_t g_fade_executing_dc = 0;
thread_local FadeDcOrigin g_fade_executing_origin{};

bool FadeTraceEnabled() { return PhoneTraceConfig().fade && IsNativeMode(); }
bool FadeViewportCaptureEnabled() {
  static const bool enabled = PhoneTraceEnvUnsigned("REX_GTA4_FADE_VIEWPORT_CAPTURE", 0, 1) != 0;
  return enabled;
}

void FadeTraceLog(std::string_view point, const std::string& details) {
  const uint64_t record = g_fade_record_count.fetch_add(1, std::memory_order_relaxed);
  if (record < 16384) {
    REXLOG_INFO("gta4-native-fade: point={} {}", point, details);
  } else if (record == 16384) {
    REXLOG_ERROR("gta4-native-fade: point=incomplete reason=fade-record-budget-exhausted");
  }
}

bool FadeAutomaticCaptureEnabled() {
  const char* path = std::getenv("REX_GTA4_FADE_ARM_FILE");
  if (!path || !*path) return false;
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

void ArmFadeTraceWindow(uint8_t* base, uint32_t object, std::string_view reason) {
  if (!FadeAutomaticCaptureEnabled()) return;
  std::lock_guard lock(g_phone_arm_mutex);
  if (PhoneCaptureActive()) {
    FadeTraceLog("boundary-covered", fmt::format("run={} object={:08X} reason={} remaining={}",
        g_phone_run.load(), object, reason, g_phone_frames_remaining.load()));
    return;
  }
  if (g_phone_arm_runs >= 8) {
    FadeTraceLog("incomplete", "reason=capture-cycle-budget-exhausted");
    return;
  }
  ++g_phone_arm_runs;
  g_phone_command_count.store(0, std::memory_order_relaxed);
  const uint64_t run = uint64_t(std::chrono::system_clock::now().time_since_epoch().count());
  g_phone_guest_base.store(base, std::memory_order_relaxed);
  g_phone_run.store(run, std::memory_order_relaxed);
  g_phone_frames_remaining.store(PhoneTraceConfig().frames, std::memory_order_release);
  FadeTraceLog("armed", fmt::format("run={} cycle={} frame={} object={:08X} reason={} frames={}",
      run, g_phone_arm_runs, GetNativeLightSubmittedFrame(base), object, reason, PhoneTraceConfig().frames));
  PhoneTraceLog("arm-state", fmt::format("run={} cycle={} phone-visible={} lineage={} allow-closed={} source=fade",
      run, g_phone_arm_runs, LoadU8(base, 0x831D4C54), PhoneTraceConfig().lineage, PhoneTraceConfig().allow_closed));
}

void ObserveFadeAlpha(uint8_t* base, uint32_t object, uint32_t caller, uint32_t raw_alpha) {
  const uint32_t alpha = raw_alpha & 0xFF;
  uint32_t previous = UINT32_MAX;
  {
    std::lock_guard lock(g_fade_observation_mutex);
    if (g_fade_observations.size() >= 64 && !g_fade_observations.contains(object)) return;
    auto& state = g_fade_observations[object];
    previous = state.alpha;
    state.alpha = alpha;
  }
  if (FadeViewportCaptureEnabled() && alpha == 255 && previous != 255) ArmFadeTraceWindow(base, object, "alpha-reached-opaque");
  if (FadeViewportCaptureEnabled() && previous == 255 && alpha < 255) ArmFadeTraceWindow(base, object, "alpha-leaving-opaque");
  if (FadeViewportCaptureEnabled() && alpha == 0 && previous != 0 && previous != UINT32_MAX)
    ArmFadeTraceWindow(base, object, "alpha-reached-zero");
  if (previous != alpha || PhoneCaptureActive()) {
    FadeTraceLog("alpha", fmt::format(
        "run={} frame={} object={:08X} caller={:08X} raw={} alpha={} previous={} mode={} flags={} "
        "color={:08X} time-bits={:08X} start-bits={:08X} end-bits={:08X} would-draw={}",
        PhoneCaptureActive() ? g_phone_run.load() : 0, GetNativeLightSubmittedFrame(base), object,
        caller, raw_alpha, alpha, previous, LoadU32(base, object), LoadU8(base, object + 32),
        LoadU32(base, object + 24), LoadU32(base, object + 20), LoadU32(base, object + 12),
        LoadU32(base, object + 16), alpha != 0));
  }
}

void ObserveScreenFadeColor(uint8_t* base, uint32_t color) {
  if (g_fade_builder_kind != 2) return;
  ++g_screen_fade_emissions;
  const uint32_t object = g_fade_builder_object, alpha = color >> 24;
  uint32_t previous = UINT32_MAX;
  bool was_drawing = false;
  {
    std::lock_guard lock(g_screen_fade_mutex);
    if (g_screen_fade_states.size() >= 64 && !g_screen_fade_states.contains(object)) return;
    auto& state = g_screen_fade_states[object];
    previous = state.alpha;
    was_drawing = state.drawing;
    state.alpha = alpha;
    state.drawing = true;
  }
  if (!was_drawing && alpha) ArmFadeTraceWindow(base, object, "main-screen-fade-started");
  else if (alpha == 255 && previous != 255) ArmFadeTraceWindow(base, object, "main-screen-fade-opaque");
  else if (previous == 255 && alpha < 255) ArmFadeTraceWindow(base, object, "main-screen-fade-leaving-opaque");
  else if (alpha == 0 && previous != 0) ArmFadeTraceWindow(base, object, "main-screen-fade-zero");
  if (previous != alpha || PhoneCaptureActive()) FadeTraceLog("screen-alpha", fmt::format(
      "run={} frame={} object={:08X} producer=82252DE8 color={:08X} alpha={} previous={} "
      "active={} mode={} progression={} start-alpha={} end-alpha={} source=actual-emitted-color",
      PhoneCaptureActive() ? g_phone_run.load() : 0, GetNativeLightSubmittedFrame(base),
      object, color, alpha, previous, LoadU8(base, object), LoadU32(base, object + 20),
      LoadU8(base, object + 16), LoadU8(base, object + 40), LoadU8(base, object + 44)));
}

void TraceFadeDcReady(uint8_t* base, uint32_t dc, uint32_t creator) {
  if (!FadeTraceEnabled() || !dc ||
      LoadU32(base, dc) != 0x820091D8 || LoadU32(base, dc + 8) != 0x82369AA8) return;
  // Capture before publication: the generated finalize call can make the DC
  // visible to another thread. Identity is checked again at callback execution.
  const uint32_t color = LoadU32(base, dc + 12);
  ObserveScreenFadeColor(base, color);
  FadeDcOrigin origin{};
  origin.object = g_fade_builder_object;
  origin.token = LoadU32(base, dc + 4);
  origin.color = color;
  origin.creator = creator;
  origin.kind = g_fade_builder_kind;
  origin.run = PhoneCaptureActive() ? g_phone_run.load() : 0;
  origin.event = PhoneCaptureActive() ? NextPhoneEvent() : 0;
  {
    std::lock_guard lock(g_fade_dc_mutex);
    if (g_fade_dc_origins.size() >= 4096) {
      FadeTraceLog("incomplete", "reason=fade-dc-origin-capacity");
      return;
    }
    g_fade_dc_origins[dc] = origin;
  }
  if (PhoneCaptureActive()) FadeTraceLog("dc-ready", fmt::format(
      "run={} event={} frame={} object={:08X} dc={:08X} token={:08X} vtable=82009098 "
      "callback=82369F08 color={:08X} creator={:08X} kind={} publication=before-original-finalize",
      origin.run, origin.event, GetNativeLightSubmittedFrame(base), origin.object, dc,
      origin.token, origin.color, origin.creator, origin.kind));
}
}  // namespace

extern "C" void sub_82345FD8(PPCContext& ctx, uint8_t* base) {
  const uint32_t object = ctx.r3.u32, caller = uint32_t(ctx.lr);
  __imp__sub_82345FD8(ctx, base);
  if (FadeTraceEnabled() && caller == 0x82346918)
    ObserveFadeAlpha(base, object, caller, ctx.r3.u32);
}

extern "C" void sub_82346900(PPCContext& ctx, uint8_t* base) {
  if (!FadeTraceEnabled()) { __imp__sub_82346900(ctx, base); return; }
  const uint32_t previous = g_fade_builder_object;
  g_fade_builder_object = ctx.r3.u32;
  const auto previous_kind = g_fade_builder_kind;
  g_fade_builder_kind = 1;
  ScopedPhoneTrace scope(PhoneTraceScope::kScreenFadeBuilder, base, ctx.r3.u32, uint32_t(ctx.lr));
  __imp__sub_82346900(ctx, base);
  g_fade_builder_object = previous;
  g_fade_builder_kind = previous_kind;
}

extern "C" void sub_82346718(PPCContext& ctx, uint8_t* base) {
  if (!FadeTraceEnabled()) { __imp__sub_82346718(ctx, base); return; }
  const uint32_t object = ctx.r3.u32, caller = uint32_t(ctx.lr);
  const uint32_t before_mode = LoadU32(base, object), before_flags = LoadU8(base, object + 32);
  __imp__sub_82346718(ctx, base);
  const uint32_t after_mode = LoadU32(base, object), after_flags = LoadU8(base, object + 32);
  // .17:26372-26428 can set mode to -1 and clear active bits without a final
  // zero-alpha draw dispatch. Observe that real retirement, not a guessed alpha.
  if (before_mode == 0 && (before_flags & 0x60) &&
      (after_mode != 0 || !(after_flags & 0x60))) {
    if (FadeViewportCaptureEnabled()) ArmFadeTraceWindow(base, object, "fade-update-retired");
    FadeTraceLog("update-retired", fmt::format(
        "run={} frame={} object={:08X} caller={:08X} before-mode={:08X} before-flags={} "
        "after-mode={:08X} after-flags={} time-bits={:08X}",
        PhoneCaptureActive() ? g_phone_run.load() : 0, GetNativeLightSubmittedFrame(base),
        object, caller, before_mode, before_flags, after_mode, after_flags,
        LoadU32(base, object + 20)));
  }
}

extern "C" void sub_823469F8(PPCContext& ctx, uint8_t* base) {
  if (!FadeTraceEnabled()) { __imp__sub_823469F8(ctx, base); return; }
  const uint32_t object = ctx.r3.u32, phase = ctx.r4.u8, caller = uint32_t(ctx.lr);
  const uint32_t mode = LoadU32(base, object), flags = LoadU8(base, object + 32);
  if (!phase && !mode) {
    uint32_t previous = UINT32_MAX;
    {
      std::lock_guard lock(g_fade_observation_mutex);
      if (g_fade_observations.size() < 64 || g_fade_observations.contains(object)) {
        auto& state = g_fade_observations[object];
        previous = state.dispatch;
        state.dispatch = flags;
      }
    }
    if (FadeViewportCaptureEnabled() && previous != UINT32_MAX && (previous & 0x60) && !(flags & 0x60))
      ArmFadeTraceWindow(base, object, "fade-dispatch-deactivated");
    if (previous != flags || PhoneCaptureActive()) FadeTraceLog("dispatch", fmt::format(
        "run={} frame={} object={:08X} caller={:08X} phase={} mode={} flags={} previous={} active-bits={}",
        PhoneCaptureActive() ? g_phone_run.load() : 0, GetNativeLightSubmittedFrame(base), object,
        caller, phase, mode, flags, previous, flags & 0x60));
  }
  __imp__sub_823469F8(ctx, base);
}

extern "C" void sub_82265030(PPCContext& ctx, uint8_t* base) {
  if (!FadeTraceEnabled() || !ctx.r3.u32 || LoadU32(base, ctx.r3.u32 + 8) != 0x82369AA8) {
    __imp__sub_82265030(ctx, base);
    return;
  }
  const uint32_t dc = ctx.r3.u32, token = LoadU32(base, dc + 4), color = LoadU32(base, dc + 12);
  FadeDcOrigin origin{};
  bool stale = false;
  {
    std::lock_guard lock(g_fade_dc_mutex);
    const auto found = g_fade_dc_origins.find(dc);
    if (found != g_fade_dc_origins.end()) {
      // .0:15648-15651 finalize updates only token bits 0x0003FF80.
      // The constructor sequence and low flag bits remain invariant.
      if ((found->second.token & 0xFFFC007F) == (token & 0xFFFC007F) &&
          found->second.color == color) origin = found->second;
      else stale = true;
      g_fade_dc_origins.erase(found);
    }
  }
  const auto previous_origin = g_fade_executing_origin;
  const auto previous_dc = g_fade_executing_dc;
  g_fade_executing_origin = origin;
  g_fade_executing_dc = dc;
  ScopedPhoneTrace scope(PhoneTraceScope::kScreenColorDraw, base, dc, uint32_t(ctx.lr));
  if (PhoneCaptureActive()) {
    g_phone_context.dc = dc;
    g_phone_context.dc_token = token;
    g_phone_context.dc_event = origin.run == g_phone_run.load() ? origin.event : 0;
    FadeTraceLog("dc-execute", fmt::format(
        "run={} event={} frame={} dc={:08X} token={:08X} color={:08X} origin-matched={} "
        "stale-origin={} object={:08X} origin-run={} origin-event={} creator={:08X} kind={} origin-token={:08X}",
        g_phone_run.load(), g_phone_context.event, GetNativeLightSubmittedFrame(base), dc, token,
        color, origin.creator != 0, stale, origin.object, origin.run, origin.event, origin.creator, origin.kind, origin.token));
  }
  __imp__sub_82265030(ctx, base);
  g_fade_executing_dc = previous_dc;
  g_fade_executing_origin = previous_origin;
}

extern "C" void sub_82369AA8(PPCContext& ctx, uint8_t* base) {
  if (!FadeTraceEnabled()) { __imp__sub_82369AA8(ctx, base); return; }
  const uint32_t color = ctx.r3.u32, caller = uint32_t(ctx.lr);
  if (g_fade_builder_kind == 2) ObserveScreenFadeColor(base, color);
  ScopedPhoneTrace scope(PhoneTraceScope::kScreenColorDraw, base, color, caller);
  if (PhoneCaptureActive()) FadeTraceLog("screen-color-draw", fmt::format(
      "run={} event={} frame={} caller={:08X} color={:08X} alpha={} direct-fade={} builder={:08X} "
      "dc={:08X} queued-fade={} fade-object={:08X} origin-run={} origin-event={} creator={:08X} kind={}",
      g_phone_run.load(), g_phone_context.event, GetNativeLightSubmittedFrame(base), caller,
      color, color >> 24, caller == 0x823469E4, g_fade_builder_object,
      g_fade_executing_dc, g_fade_executing_origin.object != 0, g_fade_executing_origin.object,
      g_fade_executing_origin.run, g_fade_executing_origin.event, g_fade_executing_origin.creator, g_fade_executing_origin.kind));
  __imp__sub_82369AA8(ctx, base);
}

// The main screen fade is separate from the viewport-effect fade. Generated .9
// sub_822651A8 either emits the actual packed ARGB callback or emits no such draw.
extern "C" void sub_822651A8(PPCContext& ctx, uint8_t* base) {
  if (!FadeTraceEnabled()) { __imp__sub_822651A8(ctx, base); return; }
  const uint32_t object = ctx.r3.u32, caller = uint32_t(ctx.lr);
  const auto previous_object = g_fade_builder_object, previous_kind = g_fade_builder_kind;
  const auto previous_emissions = g_screen_fade_emissions;
  g_fade_builder_object = object;
  g_fade_builder_kind = 2;
  g_screen_fade_emissions = 0;
  bool was_drawing = false;
  uint32_t previous_mode = UINT32_MAX, previous_active = UINT32_MAX;
  {
    std::lock_guard lock(g_screen_fade_mutex);
    if (g_screen_fade_states.size() < 64 || g_screen_fade_states.contains(object)) {
      const auto& state = g_screen_fade_states[object];
      was_drawing = state.drawing;
      previous_mode = state.mode;
      previous_active = state.active;
    }
  }
  ScopedPhoneTrace scope(PhoneTraceScope::kScreenFadeBuilder, base, object, caller);
  __imp__sub_822651A8(ctx, base);
  const uint32_t emitted = g_screen_fade_emissions;
  const uint32_t active = LoadU8(base, object), mode = LoadU32(base, object + 20);
  if (was_drawing && !emitted) ArmFadeTraceWindow(base, object, "main-screen-fade-draw-stopped");
  if (was_drawing != bool(emitted) || previous_mode != mode || previous_active != active || PhoneCaptureActive())
    FadeTraceLog("screen-state", fmt::format(
        "run={} frame={} object={:08X} caller={:08X} producer=82252DE8 mode={} active={} "
        "progression={} start-alpha={} end-alpha={} emitted={} previous-drawing={}",
        PhoneCaptureActive() ? g_phone_run.load() : 0, GetNativeLightSubmittedFrame(base),
        object, caller, mode, active, LoadU8(base, object + 16), LoadU8(base, object + 40),
        LoadU8(base, object + 44), emitted, was_drawing));
  {
    std::lock_guard lock(g_screen_fade_mutex);
    if (g_screen_fade_states.size() < 64 || g_screen_fade_states.contains(object)) {
      auto& state = g_screen_fade_states[object];
      state.drawing = emitted != 0;
      state.mode = mode;
      state.active = active;
    }
  }
  g_screen_fade_emissions = previous_emissions;
  g_fade_builder_kind = previous_kind;
  g_fade_builder_object = previous_object;
}
