// Strong hooks retain every generated implementation and its register results.
// Sources: generated .1:51406, .4:17216/22583, .0:1032/15560/16607,
// .18:46058/47055, .46:27207, .65:23064. Never use inferred pseudocode.
extern "C" void sub_8267D6A8(PPCContext& ctx, uint8_t* base) {
  MaybeArmPhoneTrace(base);
  ScopedPhoneTrace scope(PhoneTraceScope::kWorldBuilder, base, ctx.r3.u32, uint32_t(ctx.lr));
  __imp__sub_8267D6A8(ctx, base);
}
extern "C" void sub_821742C8(PPCContext& ctx, uint8_t* base) {
  MaybeArmPhoneTrace(base);
  ScopedPhoneTrace scope(PhoneTraceScope::kPhoneBuilder, base, ctx.r3.u32, uint32_t(ctx.lr));
  __imp__sub_821742C8(ctx, base);
}
extern "C" void sub_82146780(PPCContext& ctx, uint8_t* base) {
  const uint32_t dc = ctx.r3.u32;
  GTA4_HelpTraceFinalize(base, dc, uint32_t(ctx.lr));
  TraceFadeDcReady(base, dc, uint32_t(ctx.lr));
  __imp__sub_82146780(ctx, base);
  gta4::aspect::FinalizeDc(base, dc);
  if (!PhoneCaptureActive() || !dc ||
      g_phone_context.scope != uint32_t(PhoneTraceScope::kPhoneBuilder)) return;
  const uint32_t vtable = LoadU32(base, dc);
  if (vtable != 0x82001370 && vtable != 0x820B8DC4) return;
  PhoneTraceContext origin = g_phone_context;
  origin.dc = dc;
  origin.dc_token = LoadU32(base, dc + 4);
  origin.dc_event = NextPhoneEvent();
  std::lock_guard lock(g_phone_dc_mutex);
  if (g_phone_dc_origins.size() >= 4096) {
    PhoneTraceLog("incomplete", fmt::format("run={} reason=dc-origin-capacity", origin.run));
    return;
  }
  if (g_phone_dc_origins.contains(dc))
    PhoneTraceLog("incomplete", fmt::format("run={} reason=dc-origin-replaced dc={:08X}", origin.run, dc));
  g_phone_dc_origins[dc] = origin;
  PhoneTraceLog("dc-finalized", fmt::format("run={} event={} occurrence={} dc={:08X} token={:08X} vtable={:08X}",
      origin.run, origin.dc_event, origin.occurrence, dc, origin.dc_token, vtable));
}
extern "C" void sub_821BC8F8(PPCContext& ctx, uint8_t* base) {
  const auto origin = ConsumePhoneDcOrigin(base, ctx.r3.u32);
  const auto kind = origin.dc_event ? PhoneTraceScope::kPhoneClear : PhoneTraceScope::kOutside;
  ScopedPhoneTrace scope(kind, base, ctx.r3.u32, uint32_t(ctx.lr), origin.dc_event ? &origin : nullptr);
  if (PhoneCaptureActive()) {
    PhoneTraceLog("clear-dc", fmt::format("run={} occurrence={} dc={:08X} origin={} color-enable={} depth-enable={} stencil-enable={} color={:08X} depth={:08X} stencil={}",
        g_phone_run.load(), g_phone_context.occurrence, ctx.r3.u32, origin.dc_event,
        LoadU8(base, ctx.r3.u32 + 20), LoadU8(base, ctx.r3.u32 + 21), LoadU8(base, ctx.r3.u32 + 22),
        LoadU32(base, ctx.r3.u32 + 8), LoadU32(base, ctx.r3.u32 + 12), LoadU32(base, ctx.r3.u32 + 16)));
  }
  __imp__sub_821BC8F8(ctx, base);
}
extern "C" void sub_821405D8(PPCContext& ctx, uint8_t* base) {
  const auto origin = ConsumePhoneDcOrigin(base, ctx.r3.u32);
  ScopedPhoneTrace scope(PhoneTraceScope::kPhoneModel, base, ctx.r3.u32, uint32_t(ctx.lr), &origin);
  if (PhoneCaptureActive() && !origin.dc_event)
    PhoneTraceLog("incomplete", fmt::format("run={} reason=phone-model-dc-origin-unobserved dc={:08X}", g_phone_run.load(), ctx.r3.u32));
  __imp__sub_821405D8(ctx, base);
}
extern "C" void sub_82146E88(PPCContext& ctx, uint8_t* base) {
  ScopedPhoneTrace scope(PhoneTraceScope::kPhoneModel, base, ctx.r3.u32, uint32_t(ctx.lr));
  __imp__sub_82146E88(ctx, base);
}
extern "C" void sub_8235E010(PPCContext& ctx, uint8_t* base) {
  MaybeArmPhoneTrace(base);
  ScopedPhoneTrace scope(PhoneTraceScope::kScript2DBuilder, base, ctx.r3.u32, uint32_t(ctx.lr));
  __imp__sub_8235E010(ctx, base);
}
extern "C" void sub_8235D930(PPCContext& ctx, uint8_t* base) {
  ScopedPhoneTrace scope(PhoneTraceScope::kScript2DPopulation, base, ctx.r3.u32, uint32_t(ctx.lr));
  if (PhoneCaptureActive())
    PhoneTraceLog("script2d-population", fmt::format("run={} occurrence={} phase={:08X} population-id={} phone-id={}",
        g_phone_run.load(), g_phone_context.occurrence, ctx.r3.u32,
        LoadU32(base, ctx.r3.u32 + 2352), LoadU32(base, 0x831D4C5C)));
  __imp__sub_8235D930(ctx, base);
}
extern "C" void sub_828BCDE0(PPCContext& ctx, uint8_t* base) {
  const bool trace = PhoneCaptureActive();
  const uint32_t requested = ctx.r3.u32, apply = ctx.r4.u32, caller = uint32_t(ctx.lr);
  const uint32_t before = trace ? LoadU32(base, kCurrentViewportGlobal) : 0;
  gta4::aspect::PrepareViewport(ctx, base);
  __imp__sub_828BCDE0(ctx, base);
  if (trace)
    PhoneTraceLog("viewport-transition", fmt::format("run={} occurrence={} caller={:08X} requested={:08X} apply={} before={:08X} after={:08X} returned={:08X}",
        g_phone_run.load(), g_phone_context.occurrence, caller, requested, apply, before,
        LoadU32(base, kCurrentViewportGlobal), ctx.r3.u32));
}
