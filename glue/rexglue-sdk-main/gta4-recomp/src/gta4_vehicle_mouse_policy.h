#pragma once

#include <algorithm>
#include <cstdint>

namespace gta4::input {

struct VehicleMouseActions {
  int32_t camera_x = 0;
  int32_t camera_y = 0;
  int32_t helicopter_yaw = 0;
  int32_t helicopter_pitch = 0;
};

constexpr VehicleMouseActions RouteVehicleMouse(bool helicopter_driver,
                                                bool free_look,
                                                int32_t horizontal,
                                                int32_t vertical) {
  // The PC helicopter controls reserve RMB + mouse for looking around;
  // otherwise mouse movement rotates and pitches the aircraft.
  if (helicopter_driver && !free_look) {
    return {.helicopter_yaw = horizontal, .helicopter_pitch = vertical};
  }
  return {.camera_x = horizontal, .camera_y = vertical};
}

struct VehicleYawButtons {
  uint8_t left = 0;
  uint8_t right = 0;
};

constexpr VehicleYawButtons MergeVehicleYawButtons(uint8_t left, uint8_t right,
                                                  int32_t requested) {
  // sub_822BBED0 subtracts independent button magnitudes for 57/58. They
  // are not the centered stick records used by the aircraft's pitch axis.
  const int32_t current = static_cast<int32_t>(right) - left;
  const int32_t clamped = std::clamp(requested, -255, 255);
  const int32_t current_magnitude = current < 0 ? -current : current;
  const int32_t requested_magnitude = clamped < 0 ? -clamped : clamped;
  if (requested_magnitude <= current_magnitude) {
    return {left, right};
  }
  return {static_cast<uint8_t>(clamped < 0 ? requested_magnitude : 0),
          static_cast<uint8_t>(clamped > 0 ? requested_magnitude : 0)};
}

}  // namespace gta4::input
