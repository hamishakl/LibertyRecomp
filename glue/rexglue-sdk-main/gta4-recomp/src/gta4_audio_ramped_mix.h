#pragma once

#include <array>
#include <cstdint>
#include <simde/x86/sse.h>
#include <simde/x86/ssse3.h>

namespace gta4::audio {

// sub_82199BA0 accumulates a fixed 256-sample planar block with a gain ramp.
// All guest vectors use the original reverse-byte lane convention. Separate
// multiply/add and the four interleaved gain recurrences preserve rounding.
// Caller sets the same floating-point mode as the generated implementation.
struct RampedMixState { std::array<simde__m128, 14> vectors; };

inline RampedMixState AccumulateRamped256(
    uint8_t* destination, const uint8_t* source,
    const uint8_t* initial_gain, const uint8_t* gain_step) {
#if defined(__clang__)
#pragma clang fp contract(off)
#pragma clang fp reassociate(off)
#endif
  const auto reverse = simde_mm_setr_epi8(15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0);
  const auto load = [&](const uint8_t* p) {
    return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(
        simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(p)), reverse));
  };
  const auto store = [&](uint8_t* p, simde__m128 value) {
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(p),
                         simde_mm_shuffle_epi8(simde_mm_castps_si128(value), reverse));
  };
  const auto accumulate = [](simde__m128 sample, simde__m128 gain, simde__m128 previous) {
    return simde_mm_add_ps(simde_mm_mul_ps(sample, gain), previous);
  };
  RampedMixState result{};
  auto& v = result.vectors;
  v[0] = load(gain_step);
  v[11] = simde_mm_add_ps(v[0], v[0]);
  v[13] = load(initial_gain);
  v[12] = simde_mm_add_ps(v[13], v[0]);
  v[10] = simde_mm_add_ps(v[11], v[0]);
  v[0] = simde_mm_add_ps(v[11], v[11]);
  v[11] = simde_mm_add_ps(v[13], v[11]);
  v[10] = simde_mm_add_ps(v[13], v[10]);
  for (uint32_t offset = 0; offset < 1024; offset += 64) {
    // Load all values before the stores, including overlapping source/dest.
    v[8] = v[13];
    v[9] = load(destination + offset + 48);
    v[4] = load(source + offset + 48);
    v[3] = load(source + offset);
    v[7] = v[12];
    v[6] = v[11];
    v[9] = accumulate(v[4], v[10], v[9]);
    v[2] = load(destination + offset);
    v[8] = accumulate(v[3], v[8], v[2]);
    v[5] = load(source + offset + 32);
    v[4] = load(source + offset + 16);
    v[13] = simde_mm_add_ps(v[13], v[0]);
    v[1] = load(destination + offset + 16);
    v[3] = load(destination + offset + 32);
    v[7] = accumulate(v[4], v[7], v[1]);
    v[6] = accumulate(v[5], v[6], v[3]);
    v[12] = simde_mm_add_ps(v[12], v[0]);
    v[11] = simde_mm_add_ps(v[11], v[0]);
    v[10] = simde_mm_add_ps(v[10], v[0]);
    store(destination + offset + 48, v[9]);
    store(destination + offset, v[8]);
    store(destination + offset + 16, v[7]);
    store(destination + offset + 32, v[6]);
  }
  return result;
}

}  // namespace gta4::audio
