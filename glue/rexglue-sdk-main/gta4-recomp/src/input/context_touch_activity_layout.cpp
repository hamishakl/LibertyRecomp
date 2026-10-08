#include "input/context_touch_activity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <rex/input/input.h>

namespace gta4::input {
namespace {
using K = TouchActivityKind;
using P = TouchActivityPhase;
using G = TouchActivityGesture;
using Q = TouchScriptQueryKind;
using C = ContextTouchControlKind;
using A = TouchAction;

template<size_t N> void Text(std::array<char, N>& destination, std::string_view source) {
  const size_t count = std::min(source.size(), N - 1);
  std::copy_n(source.data(), count, destination.data());
  destination[count] = 0;
}
bool SameBinding(TouchScriptControl a, TouchScriptControl b) {
  a = CanonicalTouchScriptControl(a); b = CanonicalTouchScriptControl(b);
  return a.kind == b.kind && a.action == b.action && a.input_group == b.input_group &&
         a.script_thread == b.script_thread && a.generation == b.generation;
}
std::string_view ExtraLabel(TouchScriptControl binding) {
  binding = CanonicalTouchScriptControl(binding);
  if (binding.kind == Q::kRawButton) {
    // Raw script IDs, verified at sub_825F8C38, not glyph ordinals.
    constexpr std::array raw{"L BUMPER", "L TRIGGER", "R BUMPER", "R TRIGGER",
        "UP", "DOWN", "LEFT", "RIGHT", "PAUSE", "VIEW", "X", "Y", "A", "B", "L3", "R3"};
    return binding.action >= 4 && binding.action < 20 ? raw[binding.action - 4] : "ACTION";
  }
  switch (binding.action) {
    case 1: return "SPRINT";
    case 4: return "ACTION";
    case 23: return "LEAVE";
    case 43: return "EXIT";
    case 64: return "DOWN";
    case 65: return "UP";
    case 66: return "LEFT";
    case 67: return "RIGHT";
    case 77: return "CONFIRM";
    case 78: return "BACK";
    case 79: return "X";
    case 80: return "Y";
    default: return "ACTION";
  }
}
}  // namespace

ContextTouchLayout BuildTouchActivityLayout(const ContextTouchViewport& v,
    const TouchActivitySnapshot& activity, uint64_t generation,
    std::span<const TouchScriptControl> observed, const ContextTouchLayoutOptions& options) {
  ContextTouchLayout layout{.mode = ContextTouchMode::kMinigame, .viewport = v};
  layout.activity = activity;
  layout.opacity = std::isfinite(options.opacity) ? std::clamp(options.opacity, 0.2f, 1.0f) : 0.65f;
  if (!activity.valid || !activity.script_thread || !generation || !v.valid || !v.focused ||
      !std::isfinite(v.safe_x) || !std::isfinite(v.safe_y) || !std::isfinite(v.safe_width) ||
      !std::isfinite(v.safe_height) || v.safe_width <= 0 || v.safe_height <= 0) return layout;
  const float edge = std::min(v.safe_width, v.safe_height);
  const float scale = std::isfinite(options.button_scale) ? std::clamp(options.button_scale, 0.65f, 1.8f) : 1.0f;
  const float radius = std::clamp(edge * 0.050f * scale, edge * 0.040f, edge * 0.074f);
  const float padding = edge * 0.03f;
  const float bottom = v.safe_y + v.safe_height;
  const float right = v.safe_x + v.safe_width;
  size_t buttons = 0;
  const auto query = [&](Q kind, uint32_t action, uint32_t group = 2) {
    return TouchScriptControl{kind, action, group, activity.script_thread, generation};
  };
  const auto append = [&](ContextTouchControl c) {
    if (layout.control_count < layout.controls.size()) layout.controls[layout.control_count++] = c;
  };
  const auto button = [&](TouchScriptControl binding, std::string_view name) {
    binding = CanonicalTouchScriptControl(binding);
    for (size_t i = 0; i < layout.control_count; ++i)
      if (layout.controls[i].kind == C::kScriptButton && SameBinding(layout.controls[i].script, binding)) return;
    const size_t columns = 4;
    const float step = radius * 2.25f;
    float x, y;
    for (;;) {
      x = right - padding - radius - float(buttons % columns) * step;
      y = bottom - padding - radius - float(buttons / columns) * step;
      ++buttons;
      if (x - radius < v.safe_x || y - radius < v.safe_y + v.safe_height * 0.42f) return;
      // Hi-Lo relocates and enlarges its choice buttons. Utility and observed
      // prompt buttons must use a free grid cell after those final placements.
      bool occupied = false;
      for (size_t i = 0; i < layout.control_count; ++i) {
        const auto& existing = layout.controls[i];
        if (!existing.visible || existing.kind == C::kActivitySurface) continue;
        occupied |= std::hypot(x - existing.center_x, y - existing.center_y) <
                    radius + existing.radius;
      }
      if (!occupied) break;
    }
    ContextTouchControl c;
    c.kind = C::kScriptButton; c.action = A::kScript; c.script = binding;
    c.center_x = x; c.center_y = y; c.radius = radius;
    c.minimum_x = x - radius; c.maximum_x = x + radius;
    c.minimum_y = y - radius; c.maximum_y = y + radius;
    Text(c.label, name); Text(c.accessible_name, name);
    append(c);
  };
  const auto raw = [&](uint32_t id, std::string_view name) { button(query(Q::kRawButton, id, 0), name); };
  const auto action = [&](uint32_t id, std::string_view name, uint32_t group = 2) {
    button(query(Q::kControlHeld, id, group), name);
  };
  const auto surface = [&](G gesture, std::string_view name, bool second = false) {
    ContextTouchControl c;
    c.kind = C::kActivitySurface;
    c.action = second ? A::kActivitySecondary : A::kActivityPrimary;
    c.script = query(Q::kAnalogueSticks, 0, 0);
    c.activity_gesture = gesture;
    c.minimum_x = v.safe_x + v.safe_width * (second ? 0.55f : 0.04f);
    c.maximum_x = v.safe_x + v.safe_width * (second ? 0.96f : 0.45f);
    c.minimum_y = v.safe_y + v.safe_height * 0.53f;
    c.maximum_y = v.safe_y + v.safe_height * (second ? 0.72f : 0.91f);
    c.center_x = (c.minimum_x + c.maximum_x) * 0.5f;
    c.center_y = (c.minimum_y + c.maximum_y) * 0.5f;
    c.radius = std::min(c.maximum_x - c.minimum_x, c.maximum_y - c.minimum_y) * 0.40f;
    Text(c.label, name); Text(c.accessible_name, name);
    append(c);
  };
  const auto menu = [&] {
    raw(8, "UP"); raw(9, "DOWN"); raw(10, "LEFT"); raw(11, "RIGHT");
    action(77, "CONFIRM"); action(78, "BACK");
  };
  const auto stage = activity.phase;
  switch (activity.kind) {
    case K::kBowling:
      if (stage == P::kPosition) { surface(G::kHorizontalLeft, "POSITION"); action(77, "READY"); }
      else if (stage == P::kStroke) { surface(G::kStrokeRight, "PULL / PUSH"); action(78, "REPOSITION"); }
      else if (stage == P::kAftertouch) surface(G::kRelativeRight, "ADJUST ROLL");
      else if (stage == P::kSetup || stage == P::kResult) menu();
      if (stage == P::kWaiting) action(77, "SKIP SHOT");
      raw(15, "SCORE");
      break;
    case K::kPool:
      if (stage == P::kPlaceBall) { surface(G::kRelativeLeft, "PLACE BALL"); action(77, "PLACE"); }
      else if (stage == P::kAim) {
        surface(G::kHorizontalLeft, "AIM CUE"); surface(G::kHoldRight, "CUE TIP", true);
        raw(14, "PRECISE"); raw(13, "VIEW"); action(77, "READY");
      } else if (stage == P::kStroke) {
        surface(G::kStrokeRight, "PULL / PUSH"); action(78, "AIM AGAIN"); raw(13, "VIEW");
      } else if (stage == P::kSetup || stage == P::kResult) menu();
      break;
    case K::kDarts:
      if (stage == P::kAim) {
        surface(G::kRelativeLeft, "AIM"); raw(16, "THROW"); raw(7, "STEADY"); raw(6, "FAST AIM");
        surface(G::kHoldRight, "SCORE", true);
      } else if (stage == P::kSetup || stage == P::kResult) { raw(16, "PLAY"); action(77, "CONFIRM"); }
      else if (stage == P::kWaiting) raw(16, "SKIP SHOT");
      break;
    case K::kQub3d:
      if (stage == P::kPlaying) {
        surface(G::kQub3d, "ROTATE / DROP");
        raw(10, "ROTATE LEFT"); raw(11, "ROTATE RIGHT"); raw(9, "DROP");
        // The guest owns earned charges and power-choice state. The letters
        // match its power meter rather than inventing a new ability ordering.
        raw(14, "POWER X"); raw(15, "POWER Y"); raw(16, "POWER A"); raw(17, "POWER B");
        raw(8, "UP");
      } else { menu(); raw(16, "CONTINUE"); }
      break;
    case K::kAirHockey:
      if (stage == P::kPlaying) {
        surface(G::kRelativeLeft, "MOVE MALLET"); surface(G::kStrokeRight, "POWER SHOT", true); raw(13, "VIEW");
      } else if (stage == P::kSetup || stage == P::kResult) {
        action(77, "PLAY"); raw(16, "OPPONENT A"); raw(17, "OPPONENT B"); raw(14, "OPPONENT X");
      }
      break;
    case K::kArmWrestling:
      if (stage == P::kPlaying) surface(G::kAlternatingHorizontal, "SWIPE L / R");
      else if (stage == P::kSetup || stage == P::kResult) action(77, "READY");
      break;
    case K::kHiLo:
      if (stage == P::kChoice) {
        action(78, "LOWER"); action(79, "HIGHER");
        for (size_t i = 0; i < layout.control_count; ++i) {
          auto& c = layout.controls[i];
          c.radius = edge * 0.085f;
          c.center_x = v.safe_x + v.safe_width * (i ? 0.70f : 0.30f);
          c.center_y = v.safe_y + v.safe_height * 0.79f;
          c.minimum_x = c.center_x - c.radius; c.maximum_x = c.center_x + c.radius;
          c.minimum_y = c.center_y - c.radius; c.maximum_y = c.center_y + c.radius;
        }
      } else if (stage == P::kSetup || stage == P::kResult) action(77, "CONTINUE");
      break;
    case K::kDancing:
      if (stage == P::kPlaying || stage == P::kHold) {
        // Either stick is accepted. One thumb can retain a deflected position
        // as the other taps a trigger; no automatic hold or beat alignment.
        surface(G::kHoldLeft, "DANCE"); surface(G::kHoldRight, "DANCE", true);
        if (stage == P::kHold) raw(7, "BEAT");
      } else if (stage == P::kGroup) {
        surface(G::kHoldLeft, "DIRECTION");
        raw(16, "A"); raw(17, "B"); raw(14, "X"); raw(15, "Y");
      } else if (stage == P::kSetup) { raw(16, "DANCE"); raw(15, "INSTRUCTIONS"); }
      break;
    case K::kChampagne:
      if (stage == P::kShake) { surface(G::kAlternatingVertical, "SHAKE"); action(77, "POP CORK"); }
      else if (stage == P::kSpray) surface(G::kHoldRight, "SPRAY");
      else if (stage == P::kDrink) surface(G::kCircleRight, "CIRCLE TO DRINK");
      break;
    case K::kDrinking: action(77, "DRINK"); break;
    case K::kGolf:
      if (stage == P::kSwing) {
        surface(G::kRelativeLeft, "AIM");
        if (activity.analog_golf) surface(G::kStrokeRight, "PULL / PUSH", true);
        else { action(1, "SWING", 0); surface(G::kHoldRight, "VIEW", true); }
        action(4, "FOCUS", 0);
      } else if (stage == P::kSetup || stage == P::kResult) action(77, "PLAY", 0);
      break;
    case K::kTaxi:
      surface(G::kHoldLeft, "DESTINATION"); surface(G::kHoldRight, "LOOK", true);
      action(77, stage == P::kTravel ? "SKIP TRIP" : "SELECT");
      if (stage == P::kTravel) raw(14, "HURRY");
      action(78, "BACK"); action(43, "EXIT TAXI");
      break;
    case K::kPoliceComputer:
      menu();
      if (stage == P::kChoice) raw(14, "DELETE");
      break;
    case K::kCageFighting: menu(); break;
    default: break;
  }
  if (stage == P::kQuit) { action(23, "LEAVE"); action(77, "CONTINUE"); }
  else if (activity.kind != K::kTaxi && activity.kind != K::kPoliceComputer) action(23, "LEAVE");
  // An observed, visible original prompt can expose unusual camera, betting,
  // tutorial, or mission variants without borrowing another script's controls.
  for (const auto& c : observed) {
    if (c.script_thread != activity.script_thread || c.generation != generation ||
        c.kind == Q::kAnalogueSticks || (c.kind == Q::kRawButton && c.action == 12) ||
        (c.kind != Q::kRawButton && c.action == 76)) continue;
    button(c, ExtraLabel(c));
  }
  // Native Pause stays reachable even when a minigame hides the radar.
  ContextTouchControl pause;
  pause.kind = C::kNativeButton; pause.action = A::kPause;
  pause.pad_buttons = rex::input::X_INPUT_GAMEPAD_START;
  pause.radius = radius * 0.78f;
  pause.center_x = v.safe_x + padding + pause.radius;
  pause.center_y = v.safe_y + padding + pause.radius;
  Text(pause.label, "PAUSE"); Text(pause.accessible_name, "Pause");
  append(pause);
  if (options.left_handed) {
    for (size_t i = 0; i < layout.control_count; ++i) {
      auto& c = layout.controls[i];
      c.center_x = v.safe_x + right - c.center_x;
      const float previous_min = c.minimum_x;
      c.minimum_x = v.safe_x + right - c.maximum_x;
      c.maximum_x = v.safe_x + right - previous_min;
    }
  }
  return layout;
}
}  // namespace gta4::input
