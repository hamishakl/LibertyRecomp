#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <unordered_map>
#include <utility>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/lzx.h>

#include "gta4_init.h"

namespace gta4::lzx {
namespace {

// All offsets and limits below come directly from generated sub_82A217E0,
// sub_82A21270, sub_82A21B60, and the block parser in sub_82A212F0.
constexpr uint32_t kEmbeddedDecoderOffset = 0x14;
constexpr uint32_t kInputBeginOffset = 0x2B04;
constexpr uint32_t kInputEndOffset = 0x2B08;
constexpr uint32_t kOutputOffset = 0x2B0C;
constexpr uint32_t kTotalOutputOffset = 0x2B10;
constexpr uint32_t kDecodeCountOffset = 0x2EC4;
constexpr uint32_t kOutputProtectionOffset = 0x2FE4;
constexpr uint32_t kWindowSizeOffset = 0x4;
constexpr uint32_t kRetailWindowSize = 0x20000;
constexpr uint32_t kMaximumFrameSize = 0x8000;
constexpr uint32_t kMaximumCompressedFrameSize = 0xFFFF;
constexpr uint32_t kRetailReadPadding = 0x4;
constexpr uint64_t kGuestAddressSpaceSize = uint64_t{1} << 32;
constexpr uint32_t kDecoderSpan = kOutputProtectionOffset + sizeof(uint8_t);

struct DecoderState {
  explicit DecoderState(uint32_t window_size)
      : decoder(rex::lzx::PersistentDecoder::Create(window_size)),
        input_snapshot(new (std::nothrow)
                           uint8_t[kMaximumCompressedFrameSize + kRetailReadPadding]) {}

  std::mutex mutex;
  std::unique_ptr<rex::lzx::PersistentDecoder> decoder;
  std::unique_ptr<uint8_t[]> input_snapshot;
  bool failed = false;
};

std::mutex g_registry_mutex;
std::unordered_map<uint32_t, std::shared_ptr<DecoderState>> g_registry;

// Announce at initialization, not in the per-frame decoding path. Native
// decoding is unconditional; comparison with retail lives in the test harness.
void LogBackendOnce() {
  static std::once_flag once;
  std::call_once(once,
                 [] { REXLOG_INFO("gta4-lzx: backend=libmspack mode=native-only format=xmem"); });
}

bool IsGuestRange(uint32_t address, uint64_t size) {
  if (size == 0) {
    return true;
  }
  if (address == 0 || size > kGuestAddressSpaceSize) {
    return false;
  }
  const uint64_t end = static_cast<uint64_t>(address) + size;
  return end <= kGuestAddressSpaceSize && !(address < 0xE0000000u && end > 0xE0000000ull);
}

std::shared_ptr<DecoderState> FindEntry(uint32_t decoder_address) {
  std::lock_guard lock(g_registry_mutex);
  const auto found = g_registry.find(decoder_address);
  return found == g_registry.end() ? nullptr : found->second;
}

std::shared_ptr<DecoderState> GetOrCreateEntry(uint32_t decoder_address, uint32_t window_size) {
  {
    std::lock_guard lock(g_registry_mutex);
    const auto found = g_registry.find(decoder_address);
    if (found != g_registry.end()) {
      return found->second;
    }
  }
  // Decoder/window allocation is independent across streams and does not hold
  // the registry lock. Another creator can win publication; use its entry.
  try {
    auto entry = std::make_shared<DecoderState>(window_size);
    if (!entry->decoder || !entry->input_snapshot) {
      return nullptr;
    }
    std::lock_guard lock(g_registry_mutex);
    return g_registry.try_emplace(decoder_address, std::move(entry)).first->second;
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

void RemoveEntry(uint32_t decoder_address) {
  std::shared_ptr<DecoderState> retired;
  {
    std::lock_guard lock(g_registry_mutex);
    const auto found = g_registry.find(decoder_address);
    if (found == g_registry.end()) {
      return;
    }
    retired = std::move(found->second);
    g_registry.erase(found);
  }
  // A decoder in use retains its entry; its native storage cannot be freed
  // underneath DecodeFrame. Destruction never holds the global registry lock.
}

void ResetDecoder(PPCContext& ctx, uint8_t* base) {
  const uint32_t decoder_address = ctx.r3.u32;
  const auto entry =
      IsGuestRange(decoder_address, kDecoderSpan) ? FindEntry(decoder_address) : nullptr;
  if (!entry) {
    __imp__sub_82A21798(ctx, base);
    return;
  }
  // Reset the guest counters and native dictionary under the same stream lock
  // used by decoding. Neither half of a reset may be observed independently.
  std::lock_guard lock(entry->mutex);
  __imp__sub_82A21798(ctx, base);
  entry->failed = !entry->decoder->Reset();
}

void SetRetailDecodeBookkeeping(PPCContext& ctx, uint8_t* base, uint32_t decoder_address,
                                uint32_t input_address, uint32_t compressed_size,
                                uint32_t output_address) {
  REX_STORE_U32(decoder_address + kInputBeginOffset, input_address);
  REX_STORE_U32(decoder_address + kInputEndOffset,
                input_address + compressed_size + kRetailReadPadding);
  REX_STORE_U32(decoder_address + kOutputOffset, output_address);

  rex::CallFrame protection_query(ctx);
  protection_query.ctx.r3.u64 = output_address;
  __imp__MmQueryAddressProtect(protection_query.ctx, base);
  REX_STORE_U8(decoder_address + kOutputProtectionOffset,
               (protection_query.ctx.r3.u32 & 0x600) != 0 ? 1 : 0);
}

void CompleteDecode(PPCContext& ctx, uint8_t* base, uint32_t decoder_address,
                    uint32_t bytes_written_address, bool success, uint32_t bytes_written) {
  const uint32_t count = REX_LOAD_U32(decoder_address + kDecodeCountOffset);
  REX_STORE_U32(decoder_address + kDecodeCountOffset, count + 1);
  REX_STORE_U32(bytes_written_address, success ? bytes_written : 0);
  if (success) {
    const uint32_t total = REX_LOAD_U32(decoder_address + kTotalOutputOffset);
    REX_STORE_U32(decoder_address + kTotalOutputOffset, total + bytes_written);
    ctx.r3.u64 = 0;
  } else {
    ctx.r3.u64 = 1;
  }
}

struct DecodeArguments {
  uint32_t decoder = 0;
  uint32_t expected_output = 0;
  uint32_t input = 0;
  uint32_t compressed_size = 0;
  uint32_t output = 0;
  uint32_t bytes_written = 0;
};

DecodeArguments ReadArguments(const PPCContext& ctx) {
  // r8 redundantly carries the output size at the generated call site, but
  // retail sub_82A217E0 never reads it. Deliberately ignoring r8 is required
  // for ABI fidelity; r4 is the sole decode-size argument.
  return {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r9.u32,
  };
}

bool ValidateArguments(const DecodeArguments& args) {
  const uint64_t padded_input_size =
      static_cast<uint64_t>(args.compressed_size) + kRetailReadPadding;
  const uint64_t input_end = static_cast<uint64_t>(args.input) + padded_input_size;
  return args.expected_output <= kMaximumFrameSize &&
         args.compressed_size <= kMaximumCompressedFrameSize &&
         IsGuestRange(args.decoder, kDecoderSpan) && input_end < kGuestAddressSpaceSize &&
         IsGuestRange(args.input, padded_input_size) &&
         IsGuestRange(args.output, args.expected_output) &&
         IsGuestRange(args.bytes_written, sizeof(uint32_t));
}

void DecodeFrame(PPCContext& ctx, uint8_t* base, const DecodeArguments& args) {
  if (!ValidateArguments(args)) {
    if (IsGuestRange(args.decoder, kDecoderSpan) &&
        REX_LOAD_U32(args.decoder + kWindowSizeOffset) == kRetailWindowSize) {
      if (const auto entry = GetOrCreateEntry(args.decoder, kRetailWindowSize)) {
        std::lock_guard lock(entry->mutex);
        entry->failed = true;
      }
    }
    if (IsGuestRange(args.bytes_written, sizeof(uint32_t))) {
      REX_STORE_U32(args.bytes_written, 0);
    }
    ctx.r3.u64 = 1;
    return;
  }

  const auto entry = GetOrCreateEntry(args.decoder, kRetailWindowSize);
  if (!entry) {
    SetRetailDecodeBookkeeping(ctx, base, args.decoder, args.input, args.compressed_size,
                               args.output);
    CompleteDecode(ctx, base, args.decoder, args.bytes_written, false, 0);
    return;
  }

  std::lock_guard lock(entry->mutex);
  SetRetailDecodeBookkeeping(ctx, base, args.decoder, args.input, args.compressed_size,
                             args.output);
  // Never attach a fresh native dictionary partway into an existing stream.
  // Initialization/reset hooks are the only permitted lifetime transitions.
  if (REX_LOAD_U32(args.decoder + kWindowSizeOffset) != kRetailWindowSize ||
      static_cast<uint32_t>(entry->decoder->total_output_bytes()) !=
          REX_LOAD_U32(args.decoder + kTotalOutputOffset)) {
    entry->failed = true;
  }
  if (entry->failed) {
    CompleteDecode(ctx, base, args.decoder, args.bytes_written, false, 0);
    return;
  }
  const size_t readable_input_size = static_cast<size_t>(args.compressed_size) + kRetailReadPadding;
  std::memcpy(entry->input_snapshot.get(), REX_RAW_ADDR(args.input), readable_input_size);
  const auto result = entry->decoder->DecodeFrame(
      std::span<const uint8_t>(entry->input_snapshot.get(), readable_input_size),
      std::span<uint8_t>(REX_RAW_ADDR(args.output), args.expected_output));
  if (!result) {
    entry->failed = true;
  }

  CompleteDecode(ctx, base, args.decoder, args.bytes_written, static_cast<bool>(result),
                 static_cast<uint32_t>(result.bytes_written));
}

}  // namespace
}  // namespace gta4::lzx

REX_HOOK_RAW(sub_82A217E0) {
  gta4::lzx::DecodeFrame(ctx, base, gta4::lzx::ReadArguments(ctx));
}

REX_HOOK_RAW(sub_82A21798) {
  gta4::lzx::ResetDecoder(ctx, base);
}

REX_HOOK_RAW(sub_82A21B60) {
  gta4::lzx::LogBackendOnce();
  const uint32_t outer = ctx.r3.u32;
  const uint64_t embedded = static_cast<uint64_t>(outer) + ctx.r6.u32;
  if (outer && embedded < gta4::lzx::kGuestAddressSpaceSize &&
      gta4::lzx::IsGuestRange(static_cast<uint32_t>(embedded), gta4::lzx::kDecoderSpan)) {
    // The same guest allocation can be initialized again without destruction.
    // Do not let an old dictionary or failed-stream marker cross that boundary.
    gta4::lzx::RemoveEntry(static_cast<uint32_t>(embedded));
  }
  __imp__sub_82A21B60(ctx, base);
}

REX_HOOK_RAW(sub_82A14C60) {
  const uint32_t outer_object = ctx.r3.u32;
  if (gta4::lzx::IsGuestRange(outer_object, gta4::lzx::kEmbeddedDecoderOffset + sizeof(uint32_t))) {
    gta4::lzx::RemoveEntry(outer_object + gta4::lzx::kEmbeddedDecoderOffset);
  }
  __imp__sub_82A14C60(ctx, base);
}
