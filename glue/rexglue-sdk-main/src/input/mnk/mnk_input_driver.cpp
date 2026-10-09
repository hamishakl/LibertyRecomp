/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_trace.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#if REX_PLATFORM_WIN32
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(mnk_mode, true, "Input", "Enable native keyboard/mouse input");
REXCVAR_DEFINE_BOOL(mnk_controller_emulation, false, "Input",
                    "Use legacy Xbox controller translation instead of native input");
REXCVAR_DEFINE_INT32(mnk_user_index, 0, "Input", "Controller slot (0-3) for MnK").range(0, 3);
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Native mouse sensitivity").range(0.01, 10.0);
// 2.0 chosen by feel on a MacBook trackpad (2026-10-10); 1.0 is the raw macOS-accelerated delta.
REXCVAR_DEFINE_DOUBLE(mnk_trackpad_sensitivity, 2.0, "Input", "Native trackpad sensitivity")
    .range(0.01, 10.0);
REXCVAR_DEFINE_BOOL(mnk_invert_y, false, "Input", "Invert native mouse Y axis");

REXCVAR_DEFINE_STRING(keybind_a, "Space", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(keybind_b, "Shift", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(keybind_x, "R", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(keybind_y, "E", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "RMB", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "LMB", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "Q", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "F", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "C", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "MMB", "Input/Keybinds/Controller",
                      "Right stick press");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "Up", "Input/Keybinds/Controller", "D-pad up");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "Down", "Input/Keybinds/Controller", "D-pad down");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "Left", "Input/Keybinds/Controller", "D-pad left");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "Right", "Input/Keybinds/Controller", "D-pad right");
REXCVAR_DEFINE_STRING(keybind_back, "Tab", "Input/Keybinds/Controller", "Back button");
REXCVAR_DEFINE_STRING(keybind_start, "Escape", "Input/Keybinds/Controller", "Start button");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");

namespace rex::input::mnk {

using rex::ui::VirtualKey;

namespace {

std::mutex g_active_driver_mutex;
MnkInputDriver* g_active_driver = nullptr;

}  // namespace

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {
  std::lock_guard lock(g_active_driver_mutex);
  g_active_driver = this;
}

MnkInputDriver::~MnkInputDriver() {
  {
    std::lock_guard lock(g_active_driver_mutex);
    if (g_active_driver == this) {
      g_active_driver = nullptr;
    }
  }
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  if (attached_window_) {
    rex::ui::Window* window = attached_window_;
    const auto cursor_visibility = precapture_cursor_visibility_;
    window->app_context().CallInUIThreadSynchronous([window, cursor_visibility] {
      window->SetRelativeMouseMode(false);
      window->SetCursorVisibility(cursor_visibility);
      if (window->IsMouseCaptureRequested()) {
        window->ReleaseMouse();
      }
    });
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

X_STATUS MnkInputDriver::Setup() {
  REXLOG_INFO("MnK input driver initialized");
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    attached_window_ = window;
    {
      std::lock_guard lock(state_mutex_);
      // Input drivers attach after presentation opens the window. AddListener
      // does not replay that window's initial focus notification.
      has_focus_ = window->HasFocus();
      ResetKeyboardStateLocked();
      ResetPointerMotionLocked();
      mouse_wheel_ = 0;
    }
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }

  bool release_capture = false;
  rex::ui::Window::CursorVisibility cursor_visibility = rex::ui::Window::CursorVisibility::kVisible;
  {
    std::lock_guard lock(state_mutex_);
    release_capture = mouse_captured_;
    cursor_visibility = precapture_cursor_visibility_;
    mouse_captured_ = false;
    has_focus_ = false;
    ResetKeyboardStateLocked();
    mouse_wheel_ = 0;
    ResetPointerMotionLocked();
  }
  if (release_capture) {
    window->SetRelativeMouseMode(false);
    window->SetCursorVisibility(cursor_visibility);
    if (window->IsMouseCaptureRequested()) {
      window->ReleaseMouse();
    }
  }
  window->RemoveInputListener(this);
  window->RemoveListener(this);
  attached_window_ = nullptr;
}

uint32_t MnkInputDriver::UserIndex() const {
  return static_cast<uint32_t>(REXCVAR_GET(mnk_user_index));
}

bool MnkInputDriver::IsEnabled() const {
  return REXCVAR_GET(mnk_mode);
}

bool ConsumeNativeInputState(NativeInputState* out_state) {
  if (!out_state) {
    return false;
  }
  std::lock_guard lock(g_active_driver_mutex);
  return g_active_driver && g_active_driver->ConsumeNativeState(out_state);
}

static bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val) {
  VirtualKey vk = rex::ui::ParseVirtualKey(cvar_val);
  if (vk == VirtualKey::kNone)
    return false;
  uint16_t idx = static_cast<uint16_t>(vk);
  return idx < 256 && key_down[idx];
}

static int32_t SaturatingAdd(int32_t lhs, int32_t rhs) {
  const int64_t result = static_cast<int64_t>(lhs) + static_cast<int64_t>(rhs);
  return static_cast<int32_t>(
      std::clamp(result, static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
                 static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
}

X_RESULT MnkInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  UpdateMouseCapture();
  if (!IsEnabled()) {
    std::lock_guard lock(state_mutex_);
    ResetKeyboardStateLocked();
    pointer_motion_.Reset();
    mouse_wheel_ = 0;
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!out_state) {
    // A null output is a connectivity observation, not a motion sampling
    // boundary. In particular, do not consume the pending relative motion.
    return X_ERROR_SUCCESS;
  }

  const bool active = is_active();
  std::lock_guard lock(state_mutex_);

  if (!active || !has_focus_) {
    ResetKeyboardStateLocked();
    pointer_motion_.Reset();
    mouse_wheel_ = 0;
  }

  bool sampled_keys[256] = {};
  for (size_t index = 0; index < std::size(sampled_keys); ++index) {
    auto& transitions = controller_key_transitions_[index];
    sampled_keys[index] = transitions.empty() ? key_down_[index] : transitions.front() != 0;
    if (!transitions.empty()) {
      transitions.erase(transitions.begin());
    }
  }

  X_INPUT_GAMEPAD gamepad = {};
  const bool controller_emulation = REXCVAR_GET(mnk_controller_emulation);
  const NativeControllerCompatibilityBindings compatibility_bindings =
      GetNativeControllerCompatibilityBindings();
  if (active && has_focus_ && controller_emulation) {
    for (auto& transitions : native_key_transitions_) {
      transitions.clear();
    }
    uint16_t buttons = 0;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_a)))
      buttons |= X_INPUT_GAMEPAD_A;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_b)))
      buttons |= X_INPUT_GAMEPAD_B;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_x)))
      buttons |= X_INPUT_GAMEPAD_X;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_y)))
      buttons |= X_INPUT_GAMEPAD_Y;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_left_shoulder)))
      buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_right_shoulder)))
      buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_lstick_press)))
      buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_rstick_press)))
      buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_back)))
      buttons |= X_INPUT_GAMEPAD_BACK;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_start)))
      buttons |= X_INPUT_GAMEPAD_START;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_guide)))
      buttons |= X_INPUT_GAMEPAD_GUIDE;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_dpad_up)))
      buttons |= X_INPUT_GAMEPAD_DPAD_UP;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_dpad_down)))
      buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_dpad_left)))
      buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_dpad_right)))
      buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

    gamepad.buttons = buttons;
    gamepad.left_trigger = IsBindPressed(sampled_keys, REXCVAR_GET(keybind_left_trigger)) ? 0xFF : 0;
    gamepad.right_trigger = IsBindPressed(sampled_keys, REXCVAR_GET(keybind_right_trigger)) ? 0xFF : 0;

    int32_t lx = 0;
    int32_t ly = 0;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_lstick_left)))
      lx -= INT16_MAX;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_lstick_right)))
      lx += INT16_MAX;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_lstick_up)))
      ly += INT16_MAX;
    if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_lstick_down)))
      ly -= INT16_MAX;

    // A non-null legacy-controller poll is the sampling boundary. The native
    // consumer is disabled in this mode, so it cannot steal this interval.
    // Without a caller-provided frame token, retaining this relative delta for
    // another non-null XInput poll would turn it into a held analog stick.
    const PointerMotionSample motion = pointer_motion_.Consume();
    const double sensitivity =
        motion.source == rex::ui::MouseEvent::MotionSource::kSystemAccelerated
            ? REXCVAR_GET(mnk_trackpad_sensitivity)
            : REXCVAR_GET(mnk_sensitivity);
    constexpr double kBaseScale = 200.0;
    const int32_t rx = static_cast<int32_t>(motion.delta_x * sensitivity * kBaseScale);
    const int32_t ry = static_cast<int32_t>(-motion.delta_y * sensitivity * kBaseScale);

    auto clamp16 = [](int32_t value) -> int16_t {
      return static_cast<int16_t>(
          std::clamp(value, static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
    };
    gamepad.thumb_lx = clamp16(lx);
    gamepad.thumb_ly = clamp16(ly);
    gamepad.thumb_rx = clamp16(rx);
    gamepad.thumb_ry = clamp16(ry);
  } else if (active && has_focus_) {
    gamepad = BuildNativeControllerCompatibilityGamepad(
        compatibility_bindings, sampled_keys, std::size(sampled_keys));
  }
  if (!controller_emulation) {
    TraceControllerCompatibility(compatibility_bindings, gamepad);
  }

  if (std::memcmp(&gamepad, &last_emulated_gamepad_, sizeof(gamepad)) != 0) {
    ++packet_number_;
    last_emulated_gamepad_ = gamepad;
  }

  std::memset(out_state, 0, sizeof(*out_state));
  out_state->packet_number = packet_number_;
  out_state->gamepad = gamepad;
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::TraceControllerCompatibility(
    const NativeControllerCompatibilityBindings& bindings,
    const X_INPUT_GAMEPAD& gamepad) {
  if (!rex::input::IsInputTraceEnabled()) {
    return;
  }

  auto key_name = [](VirtualKey key) {
    return rex::input::InputTraceVirtualKeyName(static_cast<uint16_t>(key));
  };
  if (!compatibility_trace_configured_ ||
      bindings != last_traced_compatibility_bindings_) {
    REXLOG_INFO(
        "input-e2e: seq={} stage=mnk-controller-compat-config mode=native-digital "
        "a={}:{} a-alias={}:{} b={}:{} b-alias={}:{} x={}:{} y={}:{} "
        "dpad-up={}:{} dpad-down={}:{} dpad-left={}:{} dpad-right={}:{} "
        "start={}:{} back={}:{} shoulder={}:{}/{}:{} trigger={}:{}/{}:{} "
        "lstick-up={}:{} lstick-down={}:{} lstick-left={}:{} lstick-right={}:{}",
        rex::input::NextInputTraceSequence(), key_name(bindings.a),
        static_cast<uint16_t>(bindings.a), key_name(bindings.a_alias),
        static_cast<uint16_t>(bindings.a_alias), key_name(bindings.b),
        static_cast<uint16_t>(bindings.b), key_name(bindings.b_alias),
        static_cast<uint16_t>(bindings.b_alias), key_name(bindings.x),
        static_cast<uint16_t>(bindings.x), key_name(bindings.y),
        static_cast<uint16_t>(bindings.y), key_name(bindings.dpad_up),
        static_cast<uint16_t>(bindings.dpad_up), key_name(bindings.dpad_down),
        static_cast<uint16_t>(bindings.dpad_down), key_name(bindings.dpad_left),
        static_cast<uint16_t>(bindings.dpad_left), key_name(bindings.dpad_right),
        static_cast<uint16_t>(bindings.dpad_right), key_name(bindings.start),
        static_cast<uint16_t>(bindings.start), key_name(bindings.back),
        static_cast<uint16_t>(bindings.back), key_name(bindings.left_shoulder),
        static_cast<uint16_t>(bindings.left_shoulder),
        key_name(bindings.right_shoulder),
        static_cast<uint16_t>(bindings.right_shoulder),
        key_name(bindings.left_trigger),
        static_cast<uint16_t>(bindings.left_trigger),
        key_name(bindings.right_trigger),
        static_cast<uint16_t>(bindings.right_trigger),
        key_name(bindings.left_stick_up),
        static_cast<uint16_t>(bindings.left_stick_up),
        key_name(bindings.left_stick_down),
        static_cast<uint16_t>(bindings.left_stick_down),
        key_name(bindings.left_stick_left),
        static_cast<uint16_t>(bindings.left_stick_left),
        key_name(bindings.left_stick_right),
        static_cast<uint16_t>(bindings.left_stick_right));
    compatibility_trace_configured_ = true;
  }

  auto trace_button = [&](VirtualKey key, const char* controller_button,
                          uint16_t mask) {
    const bool down = (static_cast<uint16_t>(gamepad.buttons) & mask) != 0;
    const bool was_down =
        (static_cast<uint16_t>(last_traced_compatibility_gamepad_.buttons) & mask) != 0;
    if ((compatibility_trace_initialized_ && down == was_down) ||
        (!compatibility_trace_initialized_ && !down)) {
      return;
    }
    const auto index = static_cast<uint16_t>(key);
    const uint64_t sequence =
        index < key_event_sequences_.size() && key_event_sequences_[index] != 0
            ? key_event_sequences_[index]
            : rex::input::NextInputTraceSequence();
    rex::input::LinkInputTraceSequence(sequence);
    REXLOG_INFO(
        "input-e2e: seq={} stage=mnk-controller-compat key={} vk={} "
        "controller={} mask={:04X} down={} buttons={:04X} packet-before={} "
        "generation={} result=published",
        sequence, key_name(key), index, controller_button, mask, down,
        static_cast<uint16_t>(gamepad.buttons), packet_number_,
        key_state_generation_);
  };
  auto most_recent_key = [&](VirtualKey first, VirtualKey second) {
    if (first == VirtualKey::kNone) {
      return second;
    }
    if (second == VirtualKey::kNone) {
      return first;
    }
    const auto first_index = static_cast<uint16_t>(first);
    const auto second_index = static_cast<uint16_t>(second);
    const uint64_t first_sequence =
        first_index < key_event_sequences_.size()
            ? key_event_sequences_[first_index]
            : 0;
    const uint64_t second_sequence =
        second_index < key_event_sequences_.size()
            ? key_event_sequences_[second_index]
            : 0;
    return second_sequence > first_sequence ? second : first;
  };
  trace_button(most_recent_key(bindings.a, bindings.a_alias), "a",
               X_INPUT_GAMEPAD_A);
  trace_button(most_recent_key(bindings.b, bindings.b_alias), "b",
               X_INPUT_GAMEPAD_B);
  trace_button(bindings.x, "x", X_INPUT_GAMEPAD_X);
  trace_button(bindings.y, "y", X_INPUT_GAMEPAD_Y);
  trace_button(bindings.dpad_up, "dpad-up", X_INPUT_GAMEPAD_DPAD_UP);
  trace_button(bindings.dpad_down, "dpad-down", X_INPUT_GAMEPAD_DPAD_DOWN);
  trace_button(bindings.dpad_left, "dpad-left", X_INPUT_GAMEPAD_DPAD_LEFT);
  trace_button(bindings.dpad_right, "dpad-right", X_INPUT_GAMEPAD_DPAD_RIGHT);
  trace_button(bindings.start, "start", X_INPUT_GAMEPAD_START);
  trace_button(bindings.back, "back", X_INPUT_GAMEPAD_BACK);
  trace_button(bindings.left_shoulder, "left-shoulder",
               X_INPUT_GAMEPAD_LEFT_SHOULDER);
  trace_button(bindings.right_shoulder, "right-shoulder",
               X_INPUT_GAMEPAD_RIGHT_SHOULDER);

  auto trace_trigger = [&](VirtualKey key, const char* controller_trigger,
                           uint8_t value, uint8_t previous) {
    if ((compatibility_trace_initialized_ && value == previous) ||
        (!compatibility_trace_initialized_ && value == 0)) {
      return;
    }
    const auto index = static_cast<uint16_t>(key);
    const uint64_t sequence =
        index < key_event_sequences_.size() && key_event_sequences_[index] != 0
            ? key_event_sequences_[index]
            : rex::input::NextInputTraceSequence();
    rex::input::LinkInputTraceSequence(sequence);
    REXLOG_INFO(
        "input-e2e: seq={} stage=mnk-controller-compat key={} vk={} "
        "controller={} value={} packet-before={} generation={} result=published",
        sequence, key_name(key), index, controller_trigger, value,
        packet_number_, key_state_generation_);
  };
  trace_trigger(bindings.left_trigger, "left-trigger", gamepad.left_trigger,
                last_traced_compatibility_gamepad_.left_trigger);
  trace_trigger(bindings.right_trigger, "right-trigger", gamepad.right_trigger,
                last_traced_compatibility_gamepad_.right_trigger);

  auto trace_axis = [&](VirtualKey negative, VirtualKey positive,
                        VirtualKey previous_negative, VirtualKey previous_positive,
                        const char* controller_axis, int16_t value, int16_t previous) {
    if ((compatibility_trace_initialized_ && value == previous) ||
        (!compatibility_trace_initialized_ && value == 0)) {
      return;
    }
    VirtualKey key = most_recent_key(negative, positive);
    if (key == VirtualKey::kNone) {
      // A context change can unbind a held axis without a new host event.
      key = most_recent_key(previous_negative, previous_positive);
    }
    const auto index = static_cast<uint16_t>(key);
    const uint64_t sequence =
        index < key_event_sequences_.size() && key_event_sequences_[index] != 0
            ? key_event_sequences_[index]
            : rex::input::NextInputTraceSequence();
    rex::input::LinkInputTraceSequence(sequence);
    REXLOG_INFO(
        "input-e2e: seq={} stage=mnk-controller-compat key={} vk={} "
        "controller={} value={} previous={} packet-before={} generation={} result=published",
        sequence, key_name(key), index, controller_axis, value, previous,
        packet_number_, key_state_generation_);
  };
  trace_axis(bindings.left_stick_left, bindings.left_stick_right,
             last_traced_compatibility_bindings_.left_stick_left,
             last_traced_compatibility_bindings_.left_stick_right,
             "left-stick-x", gamepad.thumb_lx, last_traced_compatibility_gamepad_.thumb_lx);
  trace_axis(bindings.left_stick_down, bindings.left_stick_up,
             last_traced_compatibility_bindings_.left_stick_down,
             last_traced_compatibility_bindings_.left_stick_up,
             "left-stick-y", gamepad.thumb_ly, last_traced_compatibility_gamepad_.thumb_ly);

  last_traced_compatibility_bindings_ = bindings;
  last_traced_compatibility_gamepad_ = gamepad;
  compatibility_trace_initialized_ = true;
}

X_RESULT MnkInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* /*vibration*/) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // Mouse and keyboard input has no physical force-feedback output. Don't
  // claim success and mask a real controller driver's rumble failure.
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT MnkInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  ks.user_index = static_cast<uint8_t>(UserIndex());
  ks.hid_code = 0;
  keystroke_queue_.push(ks);
}

void MnkInputDriver::CenterCursor() {
  if (!attached_window_)
    return;
  int32_t cx = static_cast<int32_t>(attached_window_->GetActualLogicalWidth() / 2);
  int32_t cy = static_cast<int32_t>(attached_window_->GetActualLogicalHeight() / 2);
  prev_mouse_x_ = cx;
  prev_mouse_y_ = cy;
#if REX_PLATFORM_WIN32
  HWND hwnd = static_cast<HWND>(attached_window_->GetNativeWindowHandle());
  if (hwnd) {
    POINT pt = {static_cast<LONG>(cx), static_cast<LONG>(cy)};
    ClientToScreen(hwnd, &pt);
    SetCursorPos(pt.x, pt.y);
  }
#endif
}

void MnkInputDriver::UpdateMouseCapture() {
  if (!attached_window_)
    return;

  bool should_capture = false;
  bool currently_captured = false;
  {
    std::lock_guard lock(state_mutex_);
    should_capture = IsEnabled() && has_focus_ && is_active();
    currently_captured = mouse_captured_;
  }

  if (should_capture != currently_captured) {
    rex::ui::Window* window = attached_window_;
    if (!window->app_context().CallInUIThreadSynchronous([this, window, should_capture] {
          {
            std::lock_guard lock(state_mutex_);
            if (attached_window_ != window || mouse_captured_ == should_capture ||
                (should_capture && (!IsEnabled() || !has_focus_ || !is_active()))) {
              return;
            }
          }

          bool applied = true;
          if (should_capture) {
            precapture_cursor_visibility_ = window->GetCursorVisibility();
            applied = window->SetRelativeMouseMode(true);
            if (applied) {
              window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
              window->CaptureMouse();
            } else {
              window->SetCursorVisibility(precapture_cursor_visibility_);
              if (window->IsMouseCaptureRequested()) {
                window->ReleaseMouse();
              }
            }
          } else {
            window->SetRelativeMouseMode(false);
            window->SetCursorVisibility(precapture_cursor_visibility_);
            if (window->IsMouseCaptureRequested()) {
              window->ReleaseMouse();
            }
          }

          std::lock_guard lock(state_mutex_);
          mouse_captured_ = should_capture && applied;
          ResetPointerMotionLocked();
          if (rex::input::IsInputTraceEnabled()) {
            REXLOG_INFO(
                "input-e2e: seq={} stage=focus-capture owner=mnk mouse-capture={} "
                "requested={} applied={} reset-generation={}",
                rex::input::NextInputTraceSequence(), mouse_captured_, should_capture,
                applied, mouse_reset_generation_);
          }
        })) {
      REXLOG_ERROR("Unable to dispatch mouse capture transition to the UI thread");
    }
  }

  // Win32 has no relative-mode implementation in the common window layer.
#if REX_PLATFORM_WIN32
  bool captured_after_transition = false;
  {
    std::lock_guard lock(state_mutex_);
    captured_after_transition = mouse_captured_;
  }
  if (captured_after_transition) {
    CenterCursor();
  }
#endif
}

void MnkInputDriver::ResetPointerMotionLocked() {
  pointer_motion_.Reset();
  ++mouse_reset_generation_;
}

void MnkInputDriver::ResetKeyboardStateLocked() {
  const bool changed =
      std::any_of(std::begin(key_down_), std::end(key_down_), [](bool value) { return value; }) ||
      std::any_of(native_key_transitions_.begin(), native_key_transitions_.end(),
                  [](const auto& transitions) { return !transitions.empty(); }) ||
      std::any_of(controller_key_transitions_.begin(), controller_key_transitions_.end(),
                  [](const auto& transitions) { return !transitions.empty(); });
  std::memset(key_down_, 0, sizeof(key_down_));
  for (auto& transitions : native_key_transitions_) {
    transitions.clear();
  }
  for (auto& transitions : controller_key_transitions_) {
    transitions.clear();
  }
  if (changed) {
    ++key_state_generation_;
  }
}

bool MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    if (key_down_[vk] == down) {
      return false;
    }
    key_down_[vk] = down;
    native_key_transitions_[vk].push_back(down);
    controller_key_transitions_[vk].push_back(down);
    ++key_state_generation_;
    return true;
  }
  return false;
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!IsEnabled()) {
    std::lock_guard lock(state_mutex_);
    ResetKeyboardStateLocked();
    if (rex::input::IsInputTraceEnabled()) {
      REXLOG_INFO("input-e2e: seq={} stage=mnk-key direction=down result=disabled vk={}",
                  e.input_trace_sequence(), static_cast<uint32_t>(e.virtual_key()));
    }
    return;
  }
  const bool active = is_active();
  std::lock_guard lock(state_mutex_);
  if (!active || !has_focus_) {
    ResetKeyboardStateLocked();
    if (rex::input::IsInputTraceEnabled()) {
      REXLOG_INFO("input-e2e: seq={} stage=mnk-key direction=down result=no-focus vk={}",
                  e.input_trace_sequence(), static_cast<uint32_t>(e.virtual_key()));
    }
    return;
  }
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  if (vk >= std::size(key_down_) || (e.prev_state() && !key_down_[vk])) {
    // A key held while an overlay owned input must be released and pressed
    // again; OS repeat events cannot resurrect the discarded game action.
    return;
  }
  const bool changed = SetKeyState(vk, true);
  last_key_event_sequence_ = e.input_trace_sequence();
  if (vk < key_event_sequences_.size()) {
    key_event_sequences_[vk] = e.input_trace_sequence();
  }
  if (rex::input::IsInputTraceEnabled()) {
    REXLOG_INFO(
        "input-e2e: seq={} stage=mnk-key key={} vk={} direction=down "
        "result=accepted changed={} generation={} focus={}",
        e.input_trace_sequence(), rex::input::InputTraceVirtualKeyName(vk), vk,
        changed, key_state_generation_, has_focus_);
  }
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  // Releases must retire held state even if input was disabled after key-down.
  std::lock_guard lock(state_mutex_);
  if (!IsEnabled()) {
    ResetKeyboardStateLocked();
    return;
  }
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  const bool changed = SetKeyState(vk, false);
  last_key_event_sequence_ = e.input_trace_sequence();
  if (vk < key_event_sequences_.size()) {
    key_event_sequences_[vk] = e.input_trace_sequence();
  }
  if (rex::input::IsInputTraceEnabled()) {
    REXLOG_INFO(
        "input-e2e: seq={} stage=mnk-key key={} vk={} direction=up "
        "result=accepted changed={} generation={} focus={}",
        e.input_trace_sequence(), rex::input::InputTraceVirtualKeyName(vk), vk,
        changed, key_state_generation_, has_focus_);
  }
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  const bool active = IsEnabled() && is_active();
  std::lock_guard lock(state_mutex_);
  if (!active || !has_focus_) {
    ResetKeyboardStateLocked();
    return;
  }
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      break;
    case rex::ui::MouseEvent::Button::kX1:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kXButton1), true);
      break;
    case rex::ui::MouseEvent::Button::kX2:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kXButton2), true);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  std::lock_guard lock(state_mutex_);
  if (!IsEnabled()) {
    ResetKeyboardStateLocked();
    return;
  }
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      break;
    case rex::ui::MouseEvent::Button::kX1:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kXButton1), false);
      break;
    case rex::ui::MouseEvent::Button::kX2:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kXButton2), false);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  const bool active = IsEnabled() && is_active();
  std::lock_guard lock(state_mutex_);
  if (!active || !has_focus_) {
    prev_mouse_x_ = e.x();
    prev_mouse_y_ = e.y();
    pointer_motion_.Reset();
    return;
  }
  int32_t x = e.x();
  int32_t y = e.y();
  if (e.has_relative_delta()) {
    pointer_motion_.Add(e.motion_source(), e.delta_x(), e.delta_y());
  } else {
    pointer_motion_.Add(rex::ui::MouseEvent::MotionSource::kGeneric, x - prev_mouse_x_,
                        y - prev_mouse_y_);
  }
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
}

void MnkInputDriver::OnMouseWheel(rex::ui::MouseEvent& e) {
  const bool active = IsEnabled() && is_active();
  std::lock_guard lock(state_mutex_);
  if (!active || !has_focus_)
    return;
  mouse_wheel_ = SaturatingAdd(mouse_wheel_, e.scroll_y());
}

bool MnkInputDriver::ConsumeNativeState(NativeInputState* out_state) {
  // Don't perform a synchronous UI-thread capture transition here. This method
  // is called while the global active-driver lifetime lock is held; waiting on
  // the UI thread there could deadlock against driver destruction. GetState
  // owns capture transitions before the guest consumes this snapshot.
  const bool enabled = IsEnabled();
  const bool emulated = REXCVAR_GET(mnk_controller_emulation);
  const bool active = is_active();
  std::lock_guard lock(state_mutex_);
  const bool allowed = out_state && enabled && !emulated && active && has_focus_;
  if (rex::input::IsInputTraceEnabled() &&
      last_traced_consume_status_ != static_cast<int32_t>(allowed)) {
    const uint64_t trace_sequence = last_key_event_sequence_ != 0
                                        ? last_key_event_sequence_
                                        : rex::input::NextInputTraceSequence();
    REXLOG_INFO(
        "input-e2e: seq={} stage=mnk-snapshot result={} enabled={} emulated={} active={} "
        "focus={} generation={}",
        trace_sequence, allowed ? "accepted" : "rejected", enabled, emulated, active,
        has_focus_, key_state_generation_);
    last_traced_consume_status_ = static_cast<int32_t>(allowed);
  }
  if (!allowed) {
    if (!enabled || !active || !has_focus_) {
      ResetKeyboardStateLocked();
      pointer_motion_.Reset();
      mouse_wheel_ = 0;
    } else if (emulated) {
      for (auto& transitions : native_key_transitions_) {
        transitions.clear();
      }
    }
    return false;
  }
  for (size_t index = 0; index < out_state->keys.size(); ++index) {
    auto& transitions = native_key_transitions_[index];
    out_state->keys[index] = transitions.empty() ? key_down_[index] : transitions.front();
    out_state->pressed_keys[index] = !transitions.empty() && transitions.front() != 0;
    if (!transitions.empty()) {
      transitions.erase(transitions.begin());
    }
  }
  out_state->user_index = UserIndex();
  // A successful native gameplay read owns exactly one accumulated interval.
  // GetState cannot consume it because controller emulation is disabled here.
  const PointerMotionSample motion = pointer_motion_.Consume();
  out_state->mouse_dx = motion.delta_x;
  out_state->mouse_dy = motion.delta_y;
  out_state->mouse_wheel = mouse_wheel_;
  out_state->mouse_source = motion.source;
  out_state->mouse_has_motion = motion.has_motion;
  out_state->mouse_sensitivity =
      motion.source == rex::ui::MouseEvent::MotionSource::kSystemAccelerated
          ? REXCVAR_GET(mnk_trackpad_sensitivity)
          : REXCVAR_GET(mnk_sensitivity);
  out_state->mouse_reset_generation = mouse_reset_generation_;
  out_state->invert_mouse_y = REXCVAR_GET(mnk_invert_y);
  out_state->last_key_event_sequence = last_key_event_sequence_;
  out_state->key_state_generation = key_state_generation_;
  out_state->key_event_sequences = key_event_sequences_;
  mouse_wheel_ = 0;
  if (rex::input::IsInputTraceEnabled() &&
      (!trace_snapshot_initialized_ ||
       last_traced_snapshot_generation_ != key_state_generation_ ||
       last_traced_snapshot_keys_ != out_state->keys || motion.has_motion ||
       out_state->mouse_wheel != 0)) {
    const uint64_t trace_sequence = last_key_event_sequence_ != 0
                                        ? last_key_event_sequence_
                                        : rex::input::NextInputTraceSequence();
    for (size_t index = 0; index < out_state->keys.size(); ++index) {
      if ((trace_snapshot_initialized_ &&
           last_traced_snapshot_keys_[index] == out_state->keys[index]) ||
          (!trace_snapshot_initialized_ && out_state->keys[index] == 0)) {
        continue;
      }
      const uint64_t key_sequence = out_state->key_event_sequences[index] != 0
                                        ? out_state->key_event_sequences[index]
                                        : trace_sequence;
      REXLOG_INFO(
          "input-e2e: seq={} stage=mnk-snapshot-key user={} generation={} "
          "key={} vk={} down={} result=published",
          key_sequence, out_state->user_index, key_state_generation_,
          rex::input::InputTraceVirtualKeyName(static_cast<uint32_t>(index)),
          index, out_state->keys[index] != 0);
    }
    REXLOG_INFO(
        "input-e2e: seq={} stage=mnk-snapshot result=published user={} generation={} "
        "w={} a={} s={} d={} return={} space={} back={} delete={} escape={} "
        "arrows={}/{}/{}/{} key-sequences=escape:{}:up:{}:down:{} mouse={}/{} wheel={}",
        trace_sequence, out_state->user_index, key_state_generation_,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kW)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kA)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kS)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kD)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kReturn)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kSpace)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kBack)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kDelete)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kEscape)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kUp)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kDown)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kLeft)] != 0,
        out_state->keys[static_cast<uint16_t>(VirtualKey::kRight)] != 0,
        out_state->key_event_sequences[static_cast<uint16_t>(VirtualKey::kEscape)],
        out_state->key_event_sequences[static_cast<uint16_t>(VirtualKey::kUp)],
        out_state->key_event_sequences[static_cast<uint16_t>(VirtualKey::kDown)],
        out_state->mouse_dx, out_state->mouse_dy, out_state->mouse_wheel);
    last_traced_snapshot_generation_ = key_state_generation_;
    last_traced_snapshot_keys_ = out_state->keys;
    trace_snapshot_initialized_ = true;
  }
  return true;
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  bool release_capture = false;
  uint64_t trace_sequence = 0;
  uint64_t key_generation = 0;
  rex::ui::Window::CursorVisibility cursor_visibility = rex::ui::Window::CursorVisibility::kVisible;
  {
    std::lock_guard lock(state_mutex_);
    has_focus_ = false;
    ResetKeyboardStateLocked();
    ResetPointerMotionLocked();
    mouse_wheel_ = 0;
    release_capture = mouse_captured_;
    cursor_visibility = precapture_cursor_visibility_;
    mouse_captured_ = false;
    trace_sequence = last_key_event_sequence_;
    key_generation = key_state_generation_;
  }
  if (rex::input::IsInputTraceEnabled()) {
    if (trace_sequence == 0) {
      trace_sequence = rex::input::NextInputTraceSequence();
    }
    REXLOG_INFO("input-e2e: seq={} stage=focus-capture owner=mnk focus=lost generation={}",
                trace_sequence, key_generation);
  }
  if (release_capture && attached_window_) {
    attached_window_->SetRelativeMouseMode(false);
    attached_window_->SetCursorVisibility(cursor_visibility);
    if (attached_window_->IsMouseCaptureRequested()) {
      attached_window_->ReleaseMouse();
    }
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = true;
  if (rex::input::IsInputTraceEnabled()) {
    const uint64_t trace_sequence = last_key_event_sequence_ != 0
                                        ? last_key_event_sequence_
                                        : rex::input::NextInputTraceSequence();
    REXLOG_INFO("input-e2e: seq={} stage=focus-capture owner=mnk focus=gained generation={}",
                trace_sequence, key_state_generation_);
  }
}

}  // namespace rex::input::mnk
