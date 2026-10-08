#pragma once

#include <cstdint>

namespace gta4::input {

enum class KeyboardWeaponRequest : uint8_t { kDirect, kNext, kPrevious };
enum class VehicleWeaponCandidateRoute : uint8_t {
  kRetail,
  kAscending,
  kRequestedSlot,
  kOriginalSlot,
};

// sub_823D5B88's vehicle operations call sub_823D56E0 at these two sites,
// then apply the retail vehicle/weapon compatibility and ammunition checks.
// Keep that validation in the generated function for both PC-only requests.
struct VehicleWeaponCandidatePolicy {
  uint32_t manager = 0;
  uint32_t original_slot = 0;
  uint32_t requested_slot = 0;
  KeyboardWeaponRequest request = KeyboardWeaponRequest::kNext;
  bool offered_requested_slot = false;
  uint32_t remaining_candidates = 11;

  VehicleWeaponCandidateRoute Route(uint32_t candidate_manager,
                                    uint32_t caller) {
    if (!manager || manager != candidate_manager ||
        (caller != 0x823D5C68 && caller != 0x823D5D20)) {
      return VehicleWeaponCandidateRoute::kRetail;
    }
    if (request == KeyboardWeaponRequest::kPrevious) {
      // The generated search has slots 0..10. End after a full pass even if
      // the original slot became unavailable during a weapon transition.
      if (!remaining_candidates) {
        return VehicleWeaponCandidateRoute::kOriginalSlot;
      }
      --remaining_candidates;
      return VehicleWeaponCandidateRoute::kAscending;
    }
    if (request == KeyboardWeaponRequest::kDirect) {
      if (offered_requested_slot) {
        // The retail loop rejected the requested slot. End its search at
        // the original selection instead of cycling to another weapon.
        return VehicleWeaponCandidateRoute::kOriginalSlot;
      }
      offered_requested_slot = true;
      return VehicleWeaponCandidateRoute::kRequestedSlot;
    }
    return VehicleWeaponCandidateRoute::kRetail;
  }
};

}  // namespace gta4::input
