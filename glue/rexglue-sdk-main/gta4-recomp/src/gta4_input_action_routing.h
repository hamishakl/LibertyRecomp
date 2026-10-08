#pragma once

#include <cstdint>

#include <rex/ui/virtual_key.h>

#include "gta4_keyboard_controller.h"

namespace gta4::input {

enum class KeyboardEscapeRoute : uint8_t {
  kPause,
  kFrontendBack,
  kPhoneBack,
};

// A held Escape belongs to the interface that received its press. The phone
// can disappear before key-up; reevaluating ownership then would turn the
// same press into a pause request. A pause menu also takes priority over a
// phone render object which remains visible behind it.
struct KeyboardEscapeState {
  KeyboardEscapeRoute route = KeyboardEscapeRoute::kPause;
  bool down = false;

  KeyboardEscapeRoute Update(bool key_down, bool key_pressed,
                             bool frontend_active, bool phone_visible) {
    if (key_down && (!down || key_pressed)) {
      route = frontend_active ? KeyboardEscapeRoute::kFrontendBack
              : phone_visible ? KeyboardEscapeRoute::kPhoneBack
                              : KeyboardEscapeRoute::kPause;
    }
    down = key_down;
    return route;
  }
};

constexpr int32_t KeyboardFrontendScroll(int32_t wheel, bool frontend_active,
                                        bool map_active) {
  if (!frontend_active || map_active || wheel == 0) {
    return 0;
  }
  return wheel > 0 ? -255 : 255;
}

// The HUD widget exists before CREATE_MOBILE_PHONE and survives destruction.
// Its visibility alone is therefore insufficient to give Escape to the phone.
// Retail ped phone paths require a created phone which is not moving offscreen.
constexpr bool PhoneOwnsKeyboard(bool created, bool moving_offscreen,
                                 bool render_visible) {
  return created && !moving_offscreen && render_visible;
}

// IS_PAUSE_MENU_ACTIVE (generated sub_82602368) excludes both retail exit
// transitions. The currently selected widget's own active bit is independent.
constexpr bool RetailPauseMenuActive(bool visible, uint32_t transition) {
  return visible && transition != 2 && transition != 6;
}

struct KeyboardInterfaceContext {
  bool frontend_active = false;
  bool phone_visible = false;
  bool map_active = false;
  bool helicopter_controls = false;
  KeyboardEscapeRoute escape_route = KeyboardEscapeRoute::kPause;
};

struct KeyboardActionBytes {
  uint8_t current = 0;
  uint8_t previous = 0;

  bool operator==(const KeyboardActionBytes&) const = default;
};

// Generated sub_828D5910 advances action +2 into +3 before polling a device.
// A frontend consumer may select an object which was not replayed, however.
// Retire only our own overlay in that case, preserving the controller bytes
// captured before injection. A real replay or an intervening guest write is
// authoritative and replaces that baseline.
struct KeyboardActionHistory {
  uint64_t epoch = 0;
  uint8_t baseline_current = 0;
  KeyboardActionBytes written{};
  bool initialized = false;

  KeyboardActionBytes Prepare(uint64_t sequence, bool replayed,
                              KeyboardActionBytes observed) {
    if (!initialized || replayed) {
      baseline_current = observed.current;
    } else if (observed != written) {
      baseline_current = observed.current;
    } else if (epoch != sequence) {
      observed.previous = written.current;
      observed.current = baseline_current;
    }
    initialized = true;
    epoch = sequence;
    written = observed;
    return observed;
  }

  void Commit(KeyboardActionBytes value) { written = value; }
};

constexpr bool IsKeyboardWasd(rex::ui::VirtualKey key) {
  using rex::ui::VirtualKey;
  return key == VirtualKey::kW || key == VirtualKey::kA ||
         key == VirtualKey::kS || key == VirtualKey::kD;
}

// Retail action indices are derived from sub_822C8E40 and consumed by
// sub_82262370. Keep the key/context decision testable independently of guest
// pointers, and share it between replay and consumer fallback injection.
constexpr bool ShouldInjectKeyboardInterfaceAction(
    uint32_t action, rex::ui::VirtualKey key,
    const KeyboardInterfaceContext& context) {
  using rex::ui::VirtualKey;
  // The MnK controller bridge publishes these keys as real controller buttons.
  // Native action injection must not synthesize a second copy of them.
  if (IsKeyboardControllerKey(key, context.helicopter_controls)) {
    return false;
  }
  switch (action) {
    case 21:  // Phone take out / expand keypad comes from the D-pad bridge.
      return false;
    case 22:  // Phone put away.
      return !context.frontend_active && context.phone_visible &&
             key == VirtualKey::kEscape &&
             context.escape_route == KeyboardEscapeRoute::kPhoneBack;
    case 64:
    case 65:
    case 66:
    case 67:
      return IsKeyboardWasd(key) && context.frontend_active;
    case 76:  // Pause.
      return context.escape_route == KeyboardEscapeRoute::kPause;
    case 77:  // Accept.
      return key == VirtualKey::kLButton && context.frontend_active &&
             !context.map_active;
    case 78:  // Back.
      if (key == VirtualKey::kEscape) {
        return context.escape_route != KeyboardEscapeRoute::kPause;
      }
      return key == VirtualKey::kXButton1 && context.frontend_active;
    case 79:
    case 80:
    case 81:
    case 82:
    case 83:
    case 84:
      return context.frontend_active;
    default:
      return false;
  }
}

enum class KeyboardActionRoute : uint8_t {
  kGameplayReplay,
  kPhoneActiveGameplayControl,
  kFrontendReplayWithConsumerFallback,
};

constexpr KeyboardActionRoute ClassifyKeyboardActionRoute(uint32_t action_index) {
  if (action_index == 21 || action_index == 22) {
    return KeyboardActionRoute::kPhoneActiveGameplayControl;
  }
  if (action_index >= 64 && action_index <= 84) {
    return KeyboardActionRoute::kFrontendReplayWithConsumerFallback;
  }
  return KeyboardActionRoute::kGameplayReplay;
}

constexpr bool IsContextAction(KeyboardActionRoute route) {
  return route != KeyboardActionRoute::kGameplayReplay;
}

constexpr bool UsesActiveGameplayControl(KeyboardActionRoute route) {
  return route == KeyboardActionRoute::kPhoneActiveGameplayControl;
}

constexpr bool NeedsFrontendConsumerFallback(KeyboardActionRoute route) {
  return route == KeyboardActionRoute::kFrontendReplayWithConsumerFallback;
}

}  // namespace gta4::input
