#include <cstdint>
#include <atomic>

#include <rex/chrono/clock.h>
#include <rex/audio/handoff_trace.h>
#include <rex/system/kernel_state.h>
#include <rex/memory.h>
#include <rex/diagnostics/gta4_transition.h>
#include <rex/thread.h>

#include "gta4_init.h"
#include "gta4_gpu_pass_context.h"

namespace {

constexpr uint32_t kLoadingActiveGlobal = 0x831D51B5;
constexpr uint32_t kLoadingReadyGlobal = 0x831D51B6;
constexpr uint32_t kLoadingCompleteGlobal = 0x831D51B7;
constexpr uint32_t kLoadingScreenIndexGlobal = 0x831D51C0;
constexpr uint32_t kLoadingAudioGateGlobal = 0x831D51C8;
constexpr uint32_t kFrontendStoredStateGlobal = 0x82C30C0C;
// Python-derived from `lis r11,-32076; addi r11,r11,-29864` in the generated
// sub_821BB260 body. Selector and cursor are fields +20 and +28.
constexpr uint32_t kCommandArenaStateGlobal = 0x82B389D8;
constexpr uint32_t kCommandArenaSelectorOffset = 20;
constexpr uint32_t kCommandArenaCursorOffset = 28;

thread_local uint32_t g_world_activation_depth = 0;
std::atomic<uint64_t> g_command_arena_generation{0};

uint64_t ReadLoadingStateBits(uint8_t* base) {
  uint64_t bits = REX_LOAD_U8(kLoadingReadyGlobal);
  bits |= uint64_t{REX_LOAD_U8(kLoadingCompleteGlobal)} << 8;
  bits |= uint64_t{REX_LOAD_U8(kLoadingAudioGateGlobal)} << 16;
  return bits;
}

}  // namespace

extern "C" void sub_821441B8(PPCContext& ctx, uint8_t* base) {
  rex::diagnostics::gta4_transition::NoteLoadingTick(
      0x821441B8, static_cast<uint32_t>(ctx.lr),
      REX_LOAD_U8(kLoadingActiveGlobal) != 0,
      REX_LOAD_U32(kLoadingScreenIndexGlobal), ReadLoadingStateBits(base));
  rex::audio::handoff::Loading(0x821441B8,uint32_t(ctx.lr),REX_LOAD_U8(kLoadingActiveGlobal)!=0,REX_LOAD_U32(kLoadingScreenIndexGlobal),ReadLoadingStateBits(base));
  __imp__sub_821441B8(ctx, base);
  rex::audio::handoff::Loading(0x821441B8,uint32_t(ctx.lr),REX_LOAD_U8(kLoadingActiveGlobal)!=0,REX_LOAD_U32(kLoadingScreenIndexGlobal),ReadLoadingStateBits(base));
  rex::diagnostics::gta4_transition::NoteLoadingTick(
      0x821441B8, static_cast<uint32_t>(ctx.lr),
      REX_LOAD_U8(kLoadingActiveGlobal) != 0,
      REX_LOAD_U32(kLoadingScreenIndexGlobal), ReadLoadingStateBits(base));
}

extern "C" void sub_8214AB18(PPCContext& ctx, uint8_t* base) {
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint32_t stored_state = REX_LOAD_U32(kFrontendStoredStateGlobal);
  rex::diagnostics::gta4_transition::NoteStateDispatch(
      true, 0x8214AB18, caller, stored_state);
  __imp__sub_8214AB18(ctx, base);
  rex::diagnostics::gta4_transition::NoteStateDispatch(
      false, 0x8214AB18, caller, REX_LOAD_U32(kFrontendStoredStateGlobal),
      stored_state);
}

extern "C" void sub_82141F50(PPCContext& ctx, uint8_t* base) {
  rex::audio::handoff::Span handoff_world("world-activation",0x82141F50,ctx.lr,ctx.r3.u32);
  rex::audio::handoff::Record("world",0x82141F50,{ctx.lr,ctx.r3.u32},"begin");
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint64_t arguments = uint64_t{ctx.r3.u32} |
                             (uint64_t{ctx.r4.u32} << 32);
  rex::diagnostics::gta4_transition::NoteWorldActivationBegin(
      0x82141F50, caller, arguments);
  ++g_world_activation_depth;
  __imp__sub_82141F50(ctx, base);
  --g_world_activation_depth;
  rex::audio::handoff::Record("world",0x82141F50,{ctx.lr,ctx.r3.u32},"end");
  rex::diagnostics::gta4_transition::NoteWorldActivationEnd(
      0x82141F50, caller, ctx.r3.u64);
}

extern "C" void sub_821457A0(PPCContext& ctx, uint8_t* base) {
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint64_t argument = ctx.r3.u64;
  const bool is_world_entry_teardown =
      g_world_activation_depth != 0 && (ctx.r3.u32 & 0xFF) == 0;
  if (is_world_entry_teardown) {
    rex::diagnostics::gta4_transition::NoteLoadingTeardown(
        true, 0x821457A0, caller, argument);
  }
  rex::audio::handoff::Span handoff_teardown("loading-teardown",0x821457A0,caller,argument);
  __imp__sub_821457A0(ctx, base);
  if (is_world_entry_teardown) {
    rex::diagnostics::gta4_transition::NoteLoadingTeardown(
        false, 0x821457A0, caller, argument);
  }
}

extern "C" void sub_821BB260(PPCContext& ctx, uint8_t* base) {
  const uint32_t requested_size = ctx.r3.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint32_t selector_before =
      REX_LOAD_U32(kCommandArenaStateGlobal + kCommandArenaSelectorOffset);
  const uint32_t cursor_before =
      REX_LOAD_U32(kCommandArenaStateGlobal + kCommandArenaCursorOffset);
  const uint32_t base_before = selector_before < 2
                                   ? REX_LOAD_U32(kCommandArenaStateGlobal +
                                                  selector_before * sizeof(uint32_t))
                                   : 0;
  const uint64_t begin_tick = rex::chrono::Clock::QueryHostTickCount();
  rex::diagnostics::gta4_transition::Record(
      rex::diagnostics::gta4_transition::EventSource::kGuest,
      rex::diagnostics::gta4_transition::EventType::kCommandArenaAllocateBegin,
      0x821BB260, caller, 0,
      rex::diagnostics::gta4_transition::kFlagBefore,
      (uint64_t{selector_before} << 32) | requested_size,
      (uint64_t{base_before} << 32) | cursor_before,
      g_command_arena_generation.load(std::memory_order_relaxed));

  __imp__sub_821BB260(ctx, base);

  const uint32_t returned_pointer = ctx.r3.u32;
  const uint32_t selector_after =
      REX_LOAD_U32(kCommandArenaStateGlobal + kCommandArenaSelectorOffset);
  const uint32_t cursor_after =
      REX_LOAD_U32(kCommandArenaStateGlobal + kCommandArenaCursorOffset);
  const bool wrapped = selector_after != selector_before || cursor_after < cursor_before;
  const uint64_t generation =
      wrapped ? g_command_arena_generation.fetch_add(1, std::memory_order_relaxed) + 1
              : g_command_arena_generation.load(std::memory_order_relaxed);
  const uint64_t elapsed =
      rex::chrono::Clock::QueryHostTickCount() - begin_tick;
  rex::diagnostics::gta4_transition::Record(
      rex::diagnostics::gta4_transition::EventSource::kGuest,
      rex::diagnostics::gta4_transition::EventType::kCommandArenaAllocateEnd,
      0x821BB260, caller, 0,
      wrapped ? rex::diagnostics::gta4_transition::kFlagStateChanged
              : rex::diagnostics::gta4_transition::kFlagAfter,
      (uint64_t{selector_after} << 32) | returned_pointer,
      (uint64_t{cursor_before} << 32) | cursor_after,
      (generation << 32) | uint32_t(elapsed));
}

extern "C" void sub_821BB158(PPCContext& ctx, uint8_t* base) {
  // This is the real deferred-DC execution boundary. The producer's phase
  // scope may have ended long before this list runs on the consuming thread.
  const gta4::gpu_pass::ScopedExecutedList gpu_pass_scope(
      ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r13.u32,
      [base](uint32_t address)->std::optional<uint32_t> {
        auto* kernel=REX_KERNEL_STATE();auto* memory=kernel?kernel->memory():nullptr;
        auto* heap=memory?memory->LookupHeap(address):nullptr;
        if(!base||!heap||address>UINT32_MAX-3||heap!=memory->LookupHeap(address+3))return {};
        const auto access=heap->QueryRangeAccess(address,address+3);
        using rex::memory::PageAccess;
        if(access!=PageAccess::kReadOnly&&access!=PageAccess::kReadWrite&&
           access!=PageAccess::kExecuteReadOnly&&access!=PageAccess::kExecuteReadWrite)return {};
        return REX_LOAD_U32(address);
      });
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint32_t argument3 = ctx.r3.u32;
  const uint32_t argument4 = ctx.r4.u32;
  const uint32_t argument5 = ctx.r5.u32;
  const uint32_t argument6 = ctx.r6.u32;
  const uint64_t generation =
      g_command_arena_generation.load(std::memory_order_acquire);
  const uint64_t begin_tick = rex::chrono::Clock::QueryHostTickCount();
  rex::diagnostics::gta4_transition::Record(
      rex::diagnostics::gta4_transition::EventSource::kGuest,
      rex::diagnostics::gta4_transition::EventType::kCommandArenaConsumeBegin,
      0x821BB158, caller, 0,
      rex::diagnostics::gta4_transition::kFlagBefore,
      (uint64_t{argument3} << 32) | argument4,
      (uint64_t{argument5} << 32) | argument6,
      (generation << 32) | rex::thread::current_thread_system_id());

  __imp__sub_821BB158(ctx, base);

  const uint64_t elapsed =
      rex::chrono::Clock::QueryHostTickCount() - begin_tick;
  rex::diagnostics::gta4_transition::Record(
      rex::diagnostics::gta4_transition::EventSource::kGuest,
      rex::diagnostics::gta4_transition::EventType::kCommandArenaConsumeEnd,
      0x821BB158, caller, 0,
      rex::diagnostics::gta4_transition::kFlagAfter, ctx.r3.u32, elapsed,
      (g_command_arena_generation.load(std::memory_order_acquire) << 32) |
          rex::thread::current_thread_system_id());
}

namespace {
std::array<char,80> HandoffGuestName(uint8_t* base,uint32_t address) {
  std::array<char,80> text{};
  auto* kernel=REX_KERNEL_STATE();auto* memory=kernel?kernel->memory():nullptr;
  auto* heap=memory?memory->LookupHeap(address):nullptr;
  if(!base||!heap||address>UINT32_MAX-79||heap!=memory->LookupHeap(address+79))return text;
  const auto access=heap->QueryRangeAccess(address,address+79);
  using rex::memory::PageAccess;
  if(access!=PageAccess::kReadOnly&&access!=PageAccess::kReadWrite&&access!=PageAccess::kExecuteReadOnly&&access!=PageAccess::kExecuteReadWrite)return text;
  for(size_t i=0;i+1<text.size();++i) {
    const auto ch=REX_LOAD_U8(address+uint32_t(i));if(!ch)break;
    if(ch<32||ch>=127)break;text[i]=char(ch);
  }
  return text;
}
}
extern "C" void sub_8259D9D8(PPCContext& ctx,uint8_t* base) {
  if(!rex::audio::handoff::Enabled()){__imp__sub_8259D9D8(ctx,base);return;}
  const auto caller=uint32_t(ctx.lr),object=ctx.r3.u32,nameptr=ctx.r4.u32,arg=ctx.r5.u32;
  const auto name=HandoffGuestName(base,nameptr);
  rex::audio::handoff::Span span("cutscene-prepare",0x8259D9D8,caller,object);
  rex::audio::handoff::Record("cutscene",0x8259D9D8,{caller,object,nameptr,arg,0},name.data());
  __imp__sub_8259D9D8(ctx,base);
  rex::audio::handoff::Record("cutscene",0x8259D9D8,{caller,object,nameptr,arg,1,ctx.r3.u32},name.data());
}
extern "C" void sub_8259DCE8(PPCContext& ctx,uint8_t* base) {
  rex::audio::handoff::Span span("cutscene-blocking-prepare",0x8259DCE8,ctx.lr,ctx.r4.u32);
  __imp__sub_8259DCE8(ctx,base);
}
extern "C" void sub_8259D7C8(PPCContext& ctx,uint8_t* base) {
  rex::audio::handoff::Span span("cutscene-stop",0x8259D7C8,ctx.r3.u32,ctx.r4.u32);
  __imp__sub_8259D7C8(ctx,base);
}
