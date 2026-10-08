#pragma once

#include <cstdint>

#include "gta4_init.h"

namespace gta4::input {

// Called by the existing sub_82163CB8 override after the retail predicate has
// run. Number keys and vehicle Q/Z use the retail on-foot action-8 or vehicle
// action-42 eligibility path, only at the call sites feeding the selector.
void MaybeForceDirectWeaponAction(PPCContext& ctx, uint8_t* base,
                                  uint32_t action_record, uint32_t caller);

}  // namespace gta4::input
