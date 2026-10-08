#include <rex/hook.h>
#include "gta4_init.h"
#include "gta4_audio_ramped_mix.h"

REX_HOOK_RAW(sub_82199BA0) {
  const uint64_t destination_register = ctx.r3.u64;
  const uint64_t source_register = ctx.r4.u64;
  const uint32_t destination = ctx.r3.u32 & ~15u;
  const uint32_t source = ctx.r4.u32 & ~15u;
  const uint32_t initial_gain = ctx.r5.u32 & ~15u;
  const uint32_t gain_step = ctx.r6.u32 & ~15u;
  const auto contiguous = [](uint32_t address, uint32_t size) {
    const uint64_t end = uint64_t(address) + size;
    return end <= 0x100000000ull && !(address < 0xE0000000u && end > 0xE0000000ull);
  };
  if (!contiguous(destination,1024) || !contiguous(source,1024) ||
      !contiguous(initial_gain,16) || !contiguous(gain_step,16)) {
    // Preserve the original address-wrap/alias behavior outside the fast domain.
    __imp__sub_82199BA0(ctx, base);
    return;
  }
  REX_STORE_U64(ctx.r1.u32 - 8, ctx.r31.u64);
  ctx.fpscr.enableFlushMode();
  const auto result = gta4::audio::AccumulateRamped256(
      REX_RAW_ADDR(destination), REX_RAW_ADDR(source),
      REX_RAW_ADDR(initial_gain), REX_RAW_ADDR(gain_step));
  // Retain the generated leaf's visible scratch-register results as well as
  // its memory output; this does not rely on callers ignoring volatile state.
  PPCVRegister* vectors[] = {&ctx.v0,&ctx.v1,&ctx.v2,&ctx.v3,&ctx.v4,&ctx.v5,&ctx.v6,
                            &ctx.v7,&ctx.v8,&ctx.v9,&ctx.v10,&ctx.v11,&ctx.v12,&ctx.v13};
  for (size_t i=0;i<result.vectors.size();++i) {
    simde_mm_store_ps(vectors[i]->f32,result.vectors[i]);
  }
  ctx.r5.u64 = destination_register - source_register;
  ctx.r10.s64 = static_cast<int64_t>(destination_register) + 1072;
  ctx.r11.s64 = static_cast<int64_t>(source_register) + 1056;
  ctx.r8.s64 = static_cast<int64_t>(destination_register) + 960;
  ctx.r7.s64 = static_cast<int64_t>(destination_register) + 976;
  ctx.r6.u64 = destination_register + 992;
  ctx.r9.s64 = 0;
  ctx.r3.s64 = -32;
  ctx.r4.s64 = -16;
  ctx.cr6.compare<uint32_t>(0,0,ctx.xer);
  ctx.r31.u64 = REX_LOAD_U64(ctx.r1.u32 - 8);
}
