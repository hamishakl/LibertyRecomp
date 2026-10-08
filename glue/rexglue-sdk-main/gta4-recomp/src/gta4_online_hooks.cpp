/**
 ******************************************************************************
 * @file        gta4_online_hooks.cpp
 * @brief       GTA IV-specific Xbox Live builder seams not exported by XAM.
 ******************************************************************************
 */

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string/utf8.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/live_compatibility.h>
#include <rex/system/xam/xsession.h>
#include <rex/system/xtypes.h>

#include "gta4_init.h"
#include "input/text_chat_team.h"
#include "input/voice_policy.h"

namespace {

using rex::X_HRESULT;
using rex::X_RESULT;

constexpr uint32_t kMaximumInviteRecipients = 32;
constexpr uint32_t kMaximumInviteMessageUnits = 128;
constexpr uint32_t kGtaVoiceManagerOffset = 6456;
constexpr uint32_t kMaximumVoicePacketBytes = 4096;
constexpr uint32_t kMaximumVoicePacketsPerTick = 32;
// Derived from the generated sub_826CABC0 invite-item constructor by
// tools/audit_gta_invite_accept_contract.py.
constexpr uint32_t kGtaInviteSessionInfoOffset = 4;
constexpr uint32_t kGtaInviteSenderXuidOffset = 128;

std::atomic<uint64_t> g_received_voice_packets{0};
std::atomic<uint64_t> g_rejected_voice_packets{0};
std::atomic<uint64_t> g_sent_voice_packets{0};
std::atomic<uint64_t> g_failed_voice_packets{0};
std::atomic<uint32_t> g_outgoing_voice_sequence{1};
std::atomic<bool> g_logged_voice_receive{false};
std::atomic<bool> g_logged_voice_send{false};
std::mutex g_voice_policy_mutex;
gta4::input::VoicePolicyState g_voice_policy;

std::optional<rex::system::xam::SessionRecord> ActiveVoiceSession(
    rex::system::xam::LiveCompatibilityRuntime* live) {
  if (!live || !live->active_session_id()) return std::nullopt;
  return live->FindSessionRoute(live->active_session_id());
}

std::optional<uint64_t> ResolveVoicePeerXuid(
    rex::system::xam::LiveCompatibilityRuntime* live, uint32_t peer_id) {
  const auto session = ActiveVoiceSession(live);
  if (!session) return std::nullopt;
  const auto member = std::ranges::find(
      session->members, peer_id,
      &rex::system::xam::SessionMember::multiplayer_peer_id);
  return member == session->members.end() ? std::nullopt
                                          : std::optional(member->xuid);
}

void SynchronizeVoicePolicy(rex::system::xam::LiveCompatibilityRuntime* live) {
  const auto session = ActiveVoiceSession(live);
  if (!live || !session || !live->voice_transport()) return;

  gta4::input::VoicePolicyState policy;
  {
    std::lock_guard lock(g_voice_policy_mutex);
    g_voice_policy.ObserveSession(session->session_id,
                                  session->previous_session_id);
    policy = g_voice_policy;
  }

  std::vector<uint64_t> relationship_mutes;
  if (auto* social = live->social_service()) {
    for (const auto& member : session->members) {
      if (member.xuid != live->identity().xuid &&
          social->QueryMute(member.xuid) ==
              rex::system::xam::CachedMuteState::kMuted) {
        relationship_mutes.push_back(member.xuid);
      }
    }
  }
  const auto team_targets = policy.team_only
                                ? gta4::input::FindTeamChatTargets(live)
                                : std::optional<std::vector<uint64_t>>{};
  (void)live->ConfigureVoiceRoute(gta4::input::ResolveVoiceRoute(
      policy, session->members, team_targets, relationship_mutes));
}

void SetVoiceManualMute(uint32_t peer_id, bool muted) {
  auto* kernel_state = REX_KERNEL_STATE();
  auto* live = kernel_state ? kernel_state->live_compatibility() : nullptr;
  const auto xuid = ResolveVoicePeerXuid(live, peer_id);
  const auto session = ActiveVoiceSession(live);
  if (!xuid || !session) return;
  {
    std::lock_guard lock(g_voice_policy_mutex);
    g_voice_policy.ObserveSession(session->session_id,
                                  session->previous_session_id);
    g_voice_policy.SetManualMute(*xuid, muted);
  }
  SynchronizeVoicePolicy(live);
}

void SetVoiceFocus(int32_t peer_id) {
  auto* kernel_state = REX_KERNEL_STATE();
  auto* live = kernel_state ? kernel_state->live_compatibility() : nullptr;
  const auto session = ActiveVoiceSession(live);
  if (!live || !session) return;
  if (peer_id >= 0 && !ResolveVoicePeerXuid(live, static_cast<uint32_t>(peer_id))) {
    return;
  }
  {
    std::lock_guard lock(g_voice_policy_mutex);
    g_voice_policy.ObserveSession(session->session_id,
                                  session->previous_session_id);
    if (peer_id < 0) {
      g_voice_policy.focused_peer_id.reset();
    } else {
      g_voice_policy.focused_peer_id = static_cast<uint32_t>(peer_id);
    }
  }
  SynchronizeVoicePolicy(live);
}

void SetVoiceTeamOnly(bool enabled) {
  auto* kernel_state = REX_KERNEL_STATE();
  auto* live = kernel_state ? kernel_state->live_compatibility() : nullptr;
  const auto session = ActiveVoiceSession(live);
  if (!live || !session) return;
  {
    std::lock_guard lock(g_voice_policy_mutex);
    g_voice_policy.ObserveSession(session->session_id,
                                  session->previous_session_id);
    g_voice_policy.team_only = enabled;
  }
  SynchronizeVoicePolicy(live);
}

bool IsGuestRangeValid(rex::memory::Memory* memory, uint32_t address, size_t size) {
  if (!address || !size || size > std::numeric_limits<uint32_t>::max()) return false;
  const uint64_t end = static_cast<uint64_t>(address) + size - 1;
  return end <= std::numeric_limits<uint32_t>::max() && memory->LookupHeap(address) &&
         memory->LookupHeap(static_cast<uint32_t>(end));
}

rex::X_RESULT SendInviteSnapshot(uint32_t user_index, uint32_t recipient_count,
                                 uint32_t recipients_ptr, uint32_t message_ptr) {
  auto* kernel_state = REX_KERNEL_STATE();
  auto* memory = kernel_state ? kernel_state->memory() : nullptr;
  auto* live = kernel_state ? kernel_state->live_compatibility() : nullptr;
  auto* social = live ? live->social_service() : nullptr;
  if (!memory || !social || !live->active_session_id()) return X_ERROR_NOT_LOGGED_ON;
  if (user_index != 0) return X_ERROR_NO_SUCH_USER;
  if (!recipient_count || recipient_count > kMaximumInviteRecipients ||
      !IsGuestRangeValid(memory, recipients_ptr,
                         static_cast<size_t>(recipient_count) * sizeof(rex::be<uint64_t>)) ||
      !IsGuestRangeValid(memory, message_ptr,
                         kMaximumInviteMessageUnits * sizeof(rex::be<char16_t>))) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const auto* guest_recipients =
      memory->TranslateVirtual<const rex::be<uint64_t>*>(recipients_ptr);
  std::vector<uint64_t> recipients;
  recipients.reserve(recipient_count);
  for (uint32_t index = 0; index < recipient_count; ++index) {
    const uint64_t xuid = guest_recipients[index];
    if (!xuid) return X_ERROR_INVALID_PARAMETER;
    recipients.push_back(xuid);
  }

  const auto* guest_message =
      memory->TranslateVirtual<const rex::be<char16_t>*>(message_ptr);
  std::u16string message;
  message.reserve(kMaximumInviteMessageUnits);
  for (uint32_t index = 0; index < kMaximumInviteMessageUnits; ++index) {
    const char16_t code_unit = guest_message[index];
    if (!code_unit) break;
    message.push_back(code_unit);
  }
  if (message.size() == kMaximumInviteMessageUnits) return X_ERROR_INVALID_PARAMETER;

  std::string encoded_message;
  try {
    encoded_message = rex::string::to_utf8(message);
  } catch (...) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const std::span<const uint8_t> custom_data(
      reinterpret_cast<const uint8_t*>(encoded_message.data()), encoded_message.size());
  return social->SendInvitations(live->active_session_id(), recipients, custom_data)
             ? X_ERROR_SUCCESS
             : X_ERROR_FUNCTION_FAILED;
}

void PumpReceivedVoice(PPCContext& parent_ctx, uint8_t* base,
                       uint32_t multiplayer_manager) {
  auto* kernel_state = REX_KERNEL_STATE();
  auto* live = kernel_state ? kernel_state->live_compatibility() : nullptr;
  auto* transport = live ? live->voice_transport() : nullptr;
  auto* runtime = rex::Runtime::instance();
  if (!transport || !runtime || !multiplayer_manager) return;

  const uint64_t active_session_id = live->active_session_id();
  if (!active_session_id) return;
  const uint32_t voice_manager = REX_LOAD_U32(multiplayer_manager + kGtaVoiceManagerOffset);
  if (!voice_manager) return;

  auto packet = transport->Receive(kMaximumVoicePacketBytes);
  if (!packet) return;

  const uint32_t guest_payload = runtime->memory()->SystemHeapAlloc(kMaximumVoicePacketBytes);
  const uint32_t guest_payload_size = runtime->memory()->SystemHeapAlloc(sizeof(uint32_t));
  if (!guest_payload || !guest_payload_size) {
    if (guest_payload) runtime->memory()->SystemHeapFree(guest_payload);
    if (guest_payload_size) runtime->memory()->SystemHeapFree(guest_payload_size);
    return;
  }

  uint32_t processed = 0;
  do {
    if (packet->session_id != active_session_id || !packet->source_xuid ||
        packet->source_xuid == live->identity().xuid || packet->payload.empty() ||
        packet->payload.size() > kMaximumVoicePacketBytes) {
      g_rejected_voice_packets.fetch_add(1, std::memory_order_relaxed);
    } else {
      std::memcpy(base + guest_payload, packet->payload.data(), packet->payload.size());
      REX_STORE_U32(guest_payload_size, static_cast<uint32_t>(packet->payload.size()));

      PPCContext voice_ctx = parent_ctx;
      voice_ctx.r3.u64 = voice_manager;
      voice_ctx.r4.u64 = packet->source_xuid;
      voice_ctx.r5.u64 = guest_payload;
      voice_ctx.r6.u64 = guest_payload_size;
      sub_82A269E8(voice_ctx, base);
      if (voice_ctx.r3.s32 < 0) {
        g_rejected_voice_packets.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_received_voice_packets.fetch_add(1, std::memory_order_relaxed);
      }
    }
    ++processed;
    packet = processed < kMaximumVoicePacketsPerTick
                 ? transport->Receive(kMaximumVoicePacketBytes)
                 : std::nullopt;
  } while (packet);

  runtime->memory()->SystemHeapFree(guest_payload_size);
  runtime->memory()->SystemHeapFree(guest_payload);
  bool expected = false;
  if (g_received_voice_packets.load(std::memory_order_relaxed) &&
      g_logged_voice_receive.compare_exchange_strong(expected, true,
                                                     std::memory_order_relaxed)) {
    REXSYS_INFO("GTA IV incoming voice is entering the retail compressed-packet queue");
  }
}

}  // namespace

extern "C" void sub_825A7EC0(PPCContext& ctx, uint8_t* base) {
  const uint32_t invitation_index = ctx.r3.u32;
  PPCContext lookup_ctx = ctx;
  __imp__sub_826CB8D8(lookup_ctx, base);
  const uint32_t invitation = lookup_ctx.r3.u32;
  if (!invitation) {
    __imp__sub_825A7EC0(ctx, base);
    return;
  }

  const auto* session_info = reinterpret_cast<const rex::system::xam::XSESSION_INFO*>(
      base + invitation + kGtaInviteSessionInfoOffset);
  const uint64_t session_id = rex::system::xam::XnkidToUint64(session_info->session_id);
  const uint64_t sender_xuid =
      REX_LOAD_U64(invitation + kGtaInviteSenderXuidOffset);
  auto* kernel_state = REX_KERNEL_STATE();
  auto* live = kernel_state ? kernel_state->live_compatibility() : nullptr;
  auto* social = live ? live->social_service() : nullptr;
  if (!session_id || !sender_xuid || !social) {
    __imp__sub_825A7EC0(ctx, base);
    return;
  }

  auto accepted = social->AcceptInvitation(session_id, sender_xuid);
  if (!accepted || !accepted->session) {
    ctx.r3.u64 = 0;
    REXSYS_WARN(
        "GTA IV explicit invite acceptance deferred index={} session={:016X} sender={:016X}",
        invitation_index, session_id, sender_xuid);
    return;
  }

  auto* writable_session_info = reinterpret_cast<rex::system::xam::XSESSION_INFO*>(
      base + invitation + kGtaInviteSessionInfoOffset);
  rex::system::xam::SessionRecordToGuestInfo(*accepted->session, *writable_session_info);
  ctx.r3.u64 = invitation_index;
  __imp__sub_825A7EC0(ctx, base);
  REXSYS_INFO(
      "GTA IV explicitly accepted invite index={} session={:016X}->{:016X} sender={:016X} "
      "result={}",
      invitation_index, session_id, accepted->session_id, sender_xuid, ctx.r3.u32);
}

extern "C" void sub_827D7D58(PPCContext& ctx, uint8_t* base) {
  const uint32_t multiplayer_manager = ctx.r3.u32;
  __imp__sub_827D7D58(ctx, base);
  auto* kernel_state = REX_KERNEL_STATE();
  SynchronizeVoicePolicy(kernel_state ? kernel_state->live_compatibility()
                                     : nullptr);
  PumpReceivedVoice(ctx, base, multiplayer_manager);
}

extern "C" void sub_827D7408(PPCContext& ctx, uint8_t* base) {
  const uint32_t payload_ptr = ctx.r5.u32;
  const uint32_t payload_size_ptr = ctx.r6.u32;
  __imp__sub_827D7408(ctx, base);

  auto* kernel_state = REX_KERNEL_STATE();
  auto* memory = kernel_state ? kernel_state->memory() : nullptr;
  auto* live = kernel_state ? kernel_state->live_compatibility() : nullptr;
  auto* transport = live ? live->voice_transport() : nullptr;
  if (!memory || !live || !transport ||
      live->config().backend != rex::system::xam::LiveBackend::kCommunity ||
      !live->active_session_id() ||
      !IsGuestRangeValid(memory, payload_size_ptr, sizeof(uint32_t))) {
    return;
  }

  const uint32_t payload_size = REX_LOAD_U32(payload_size_ptr);
  if (!payload_size) return;
  if (payload_size > kMaximumVoicePacketBytes ||
      !IsGuestRangeValid(memory, payload_ptr, payload_size)) {
    g_failed_voice_packets.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const uint32_t sequence =
      g_outgoing_voice_sequence.fetch_add(1, std::memory_order_relaxed);
  const auto* payload = memory->TranslateVirtual<const uint8_t*>(payload_ptr);
  if (!transport->Send(sequence,
                       std::span<const uint8_t>(payload, payload_size))) {
    g_failed_voice_packets.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // The dedicated community relay now owns this packet. Clear the returned
  // retail aggregate length so the same encoded talker frame cannot also be
  // delivered through the ordinary peer datagram path.
  REX_STORE_U32(payload_size_ptr, 0);
  g_sent_voice_packets.fetch_add(1, std::memory_order_relaxed);
  bool expected = false;
  if (g_logged_voice_send.compare_exchange_strong(
          expected, true, std::memory_order_relaxed)) {
    REXSYS_INFO(
        "GTA IV local talker packets are using the authenticated community voice relay");
  }
}

extern "C" void sub_825A8100(PPCContext& ctx, uint8_t* base) {
  const uint32_t peer_id = ctx.r3.u32;
  const bool muted = ctx.r4.u8 != 0;
  __imp__sub_825A8100(ctx, base);
  SetVoiceManualMute(peer_id, muted);
}

extern "C" void sub_825A8180(PPCContext& ctx, uint8_t* base) {
  const int32_t peer_id = ctx.r3.s32;
  __imp__sub_825A8180(ctx, base);
  SetVoiceFocus(peer_id);
}

extern "C" void sub_826FD9A0(PPCContext& ctx, uint8_t* base) {
  const bool enabled = ctx.r4.u8 != 0;
  __imp__sub_826FD9A0(ctx, base);
  SetVoiceTeamOnly(enabled);
}

extern "C" void sub_82A351D0(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const rex::X_RESULT result =
      SendInviteSnapshot(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  const uint32_t overlapped_ptr = ctx.r7.u32;
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(
        [result](uint32_t& extended_error, uint32_t& length) {
          extended_error = X_HRESULT_FROM_WIN32(result);
          length = 0;
          return result;
        },
        overlapped_ptr);
    ctx.r3.u64 = X_ERROR_IO_PENDING;
  } else {
    ctx.r3.u64 = result;
  }
  REXSYS_DEBUG("GTA IV invite builder sent {} recipients with result {:08X}", ctx.r4.u32,
               result);
}
