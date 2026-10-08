#ifndef GTA4_MULTIPLAYER_64_POLICY_H_
#define GTA4_MULTIPLAYER_64_POLICY_H_

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gta4::multiplayer64 {

// Audited against the retail generated functions. All derived layout values
// are checked by gta4-recomp/tools/derive_multiplayer_64_layout.py.
constexpr uint8_t kInvalidPeerId = 0xFF;
constexpr uint8_t kLegacyPeerCapacity = 16;
constexpr uint8_t kLastLegacyPeerId = 15;
constexpr uint8_t kExtendedPeerCapacity = 64;
constexpr uint8_t kLegacyCommandParticipantCapacity = 32;
constexpr uint32_t kPeerRecordSize = 64;
constexpr uint32_t kFirstUnsupportedLegacyParticipantCount = 33;
constexpr uint32_t kFreePeerListOffset = 1440;
constexpr uint32_t kFreeListHeadOffset = 0;
constexpr uint32_t kFreeListTailOffset = 4;
constexpr uint32_t kFreeListCountOffset = 8;
constexpr uint32_t kLegacyPeerPointerTableOffset = 1452;
constexpr uint32_t kGuestPointerSize = 4;
constexpr uint32_t kPeerManagerCapacityFieldOffset = 1516;
constexpr uint32_t kPeerFreeListNextOffset = 48;
constexpr uint32_t kPeerFreeListPreviousOffset = 52;
constexpr uint32_t kPeerManagerFlagsOffset = 8000;
constexpr uint8_t kPeerManagerIdentityLookupEnabledFlag = 0x40;
constexpr uint32_t kPlayerInfoGenerationCounterAddress = 0x82B61DA8;
constexpr uint32_t kLegacyPlayerInfoGenerationTableAddress = 0x82B61DB0;
constexpr uint32_t kLegacyPlayerInfoPointerTableAddress = 0x82B61DF0;
constexpr uint32_t kExtendedPlayerInfoShadowTableSize = 256;
constexpr uint32_t kPlayerInfoNetworkArrayRegisterReturnAddress = 0x826D1720;
constexpr uint32_t kPrimaryPlayerIdAddress = 0x82A938A8;
constexpr uint32_t kSecondaryPlayerIdAddress = 0x82A938AC;
constexpr uint32_t kPlayerInfoSize = 1456;
constexpr uint32_t kPlayerInfoPayloadSize = 96;
constexpr uint32_t kPlayerInfoPlayerPointerOffset = 1400;
constexpr uint32_t kPlayerInfoPlayerIdOffset = 1230;
constexpr uint32_t kPlayerInfoStateOffset = 1232;
constexpr uint32_t kPlayerEntityFirstCleanupFlagOffset = 528;
constexpr uint32_t kPlayerEntitySecondCleanupFlagOffset = 529;
constexpr uint32_t kStartGameSessionBaseAddress = 0x83109B10;
constexpr uint32_t kStartGameLiteralAddress = 0x8204B0C8;
constexpr uint32_t kStartGameFormatLiteralAddress = 0x820B9500;
constexpr uint32_t kStartGameMessageSinkOffset = 136;
constexpr uint32_t kStartGameDirectMessageFlagAddress = 0x830FAF73;
constexpr uint32_t kStartGameMessageSinkAddress = 0x83109B98;
constexpr uint32_t kTransitionFlagsOffset = 152;
constexpr uint32_t kTransitionResetSourceBit = 30;
constexpr uint32_t kTransitionFallbackStateAddress = 0x82FB0000;
constexpr uint32_t kTransitionNestedStateOffset = 544;
constexpr uint32_t kTransitionNestedStateBias = 96;
constexpr uint32_t kTransitionByteFlagsOffset = 91;
constexpr uint8_t kTransitionByteClearMask = 0x7F;
constexpr uint32_t kTransitionPlayerFlagsOffset = 564;
constexpr uint32_t kTransitionPlayerForceFlag = 0x00800000;
constexpr uint32_t kTransitionPlayerActiveMask = 0x60000000;
constexpr uint32_t kTransitionResetAccessorReturnAddress = 0x822EE328;
constexpr uint32_t kTransitionForceAccessorReturnAddress = 0x822EE868;
constexpr uint32_t kThresholdAccessorReturnAddress = 0x8216D5A8;
constexpr uint32_t kThresholdListCountOffset = 128;
constexpr uint32_t kThresholdListRecordStride = 8;
constexpr uint32_t kThresholdMatchCount = 5;
constexpr uint32_t kThresholdEventId = 41;
constexpr uint32_t kSampleCountAddress = 0x82FC961C;
constexpr uint32_t kSampleTableAddress = 0x82FD0A70;
constexpr uint32_t kSampleRecordSize = 8;
constexpr uint32_t kSampleCapacity = 16;
constexpr uint32_t kLobbyPositionAccessorReturnAddress = 0x826D4ED8;
constexpr uint32_t kLobbyClockAddress = 0x82C74E9C;
constexpr uint32_t kLobbySelectionIndexAddress = 0x82AB0480;
constexpr uint32_t kLobbySessionOffset = 4;
constexpr uint32_t kLobbyFirstScanFlagOffset = 9;
constexpr uint32_t kLobbyFirstScanTimestampOffset = 12;
constexpr uint32_t kLobbyPositionFlagOffset = 11;
constexpr uint32_t kLobbyPositionTimestampOffset = 20;
constexpr uint32_t kLobbyPositionRecordsOffset = 40;
constexpr uint32_t kLobbyPositionRecordCountOffset = 536;
constexpr uint32_t kLobbyPositionRecordStride = 32;
constexpr uint32_t kLobbyPositionVectorSize = 16;
constexpr uint32_t kLobbyPositionRetryTicks = 3000;
constexpr uint32_t kProximityStatusTableAddress = 0x8318EBB8;
constexpr uint32_t kProximityStatusRecordSize = 12;
constexpr uint32_t kProximityStatusCategoryOffset = 0;
constexpr uint32_t kProximityStatusWeightOffset = 8;
constexpr uint32_t kProximityStatusCategoryOneDenominatorWeight = 2;
constexpr uint32_t kProximityStatusCategoryTwoDenominatorWeight = 3;
constexpr uint32_t kProximityStatusTableBytes = 192;
constexpr uint32_t kProximityStatusTimestampAddress = 0x8318EB30;
constexpr uint32_t kProximityStatusRandomAddress = 0x8318EB34;
constexpr uint32_t kProximityStatusCategoryOneMultiplierAddress = 0x82000DC4;
constexpr uint32_t kProximityStatusCategoryTwoMultiplierAddress = 0x82019924;
constexpr uint32_t kProximityStatusEligibleIdReturnAddress = 0x8278CD5C;
constexpr uint32_t kNearestNetworkTimestampOffset = 180;
constexpr uint32_t kPlayerInfoUniqueValueOffset = 1384;
constexpr uint32_t kPlayerNetworkObjectOffset = 104;
constexpr uint32_t kPlayerNetworkObjectIdOffset = 1114;
constexpr uint32_t kPeerUniquenessIdentityScanReturnAddress = 0x826FF7D8;
constexpr uint32_t kPeerUniquenessIdentityInfoReturnAddress = 0x826FF808;
constexpr uint32_t kPeerUniquenessCandidateInfoReturnAddress = 0x826FF868;
constexpr uint32_t kCloneEndpointBuildReturnAddress = 0x826ECC8C;
constexpr uint32_t kCloneTypeQueryReturnAddress = 0x826ECC94;
constexpr uint32_t kCloneEndpointTypeOneFlag = 0x4000;
constexpr uint32_t kInlineLocalPeerPointerOffset = 64;
constexpr uint32_t kSessionStateOffset = 8;
constexpr uint32_t kSessionPublicSlotsOffset = 212;
constexpr uint32_t kSessionPrivateSlotsOffset = 216;
constexpr uint32_t kNetworkPreferencesReadyAddress = 0x830FAF6D;
constexpr uint32_t kNetworkPreferencesAddress = 0x8310CDB0;
constexpr uint32_t kFreeRoamGameMode = 0x10;
constexpr uint32_t kSetMaxSlotsTaskPublicOffset = 36;
constexpr uint32_t kSetMaxSlotsTaskPrivateOffset = 40;
constexpr uint32_t kSessionParticipantCountOffset = 1544;
constexpr uint32_t kSessionPendingCommandOffset = 1716;
constexpr uint32_t kSessionParticipantTableOffset = 520;
constexpr uint32_t kSessionParticipantRecordSize = 32;
constexpr uint32_t kSessionParticipantPrivateFlagOffset = 24;
constexpr uint32_t kExtendedSessionParticipantTableSize = 2048;
constexpr uint32_t kSetMaxSlotsCommandSize = 36;
constexpr uint32_t kSessionCommandTypeOffset = 4;
constexpr uint32_t kSetMaxSlotsCommandPublicOffset = 20;
constexpr uint32_t kSetMaxSlotsCommandPrivateOffset = 24;
constexpr uint32_t kSetMaxSlotsCommandType = 11;
constexpr uint32_t kLeaveCommandType = 6;
constexpr uint32_t kSessionCommandAllocatorAddress = 0x831CE348;
constexpr uint32_t kLeaveCommandAllocationSize = 1064;
constexpr uint32_t kJoinCommandAllocationSize = 288;
// CmdJoin's RTTI locator pointer is stored immediately before the vtable.
// sub_829F8368 installs 0x820A85FC in the command object; using 0x820A85F8
// dispatches RTTI data as code when sub_827C9D58 invokes vtable[0].
constexpr uint32_t kJoinCommandVtableAddress = 0x820A85FC;
constexpr uint32_t kJoinCommandRecordSize = 8;
constexpr uint32_t kExtendedJoinCommandTableSize = 512;
constexpr uint32_t kMigrationRecordSize = 48;
constexpr uint32_t kExtendedMigrationRecordTableSize = 3072;
constexpr uint32_t kMigrationTaskInlineTableOffset = 40;
constexpr uint32_t kMigrationTaskCountOffset = 1624;
constexpr uint32_t kMigrationTaskCurrentOffset = 1628;
constexpr uint32_t kInviteIdentitySize = 16;
constexpr uint32_t kMaxInviteIdentityBufferSize = 496;
constexpr uint32_t kGlobalPeerManagerAddress = 0x83109B10;
constexpr uint32_t kDispatchStateOffset = 8;
constexpr uint32_t kDispatchElementCountOffset = 20;
constexpr uint32_t kDispatchElementStateTableOffset = 32;
constexpr uint32_t kDispatchPeerStatePointerTableOffset = 40;
constexpr uint32_t kDispatchPeerMaskPointerTableOffset = 48;
constexpr uint32_t kDispatchAuthorityTableOffset = 56;
constexpr uint32_t kDispatchElementStateRecordSize = 4;
constexpr uint32_t kDispatchAuthorityRecordSize = 2;
constexpr uint32_t kDispatchElementPointerRecordSize = 8;
constexpr uint32_t kDispatchFirstPeerMaskOffset = 96;
constexpr uint32_t kDispatchSecondPeerMaskOffset = 100;
constexpr uint32_t kDispatchMessageStride = 1024;
constexpr uint32_t kDispatchMessageQueueOffset = 4;
constexpr uint32_t kDispatchMessagePayloadOffset = 32;
constexpr uint32_t kDispatchMessagePayloadCapacity = 989;
constexpr uint32_t kDispatchMessageWireHeaderBytes = 13;
constexpr uint32_t kDispatchSequenceOffset = 112;
constexpr uint32_t kDispatchSendAllOffset = 114;
constexpr uint32_t kDispatchInitializedOffset = 115;
constexpr uint32_t kDispatchResetEnabledOffset = 116;
constexpr uint32_t kNetworkArrayManagerPointerOffset = 4;
constexpr uint32_t kNetworkArrayPeerManagerOffset = 12;
constexpr uint32_t kNetworkArrayHandlerListOffset = 44;
constexpr uint32_t kNetworkArrayHandlerNodeValueOffset = 4;
constexpr uint32_t kNetworkArrayHandlerNodeNextOffset = 8;
constexpr uint32_t kLegacyNetworkEndpointTableOffset = 92;
constexpr uint32_t kFirstOverlappingNetworkEndpointOffset = 156;
constexpr uint32_t kPedNetworkPeerStateOffset = 320;
constexpr uint32_t kPedNetworkPeerStateStride = 48;
constexpr uint32_t kPedNetworkBlenderFactoryAddress = 0x82711848;
constexpr uint32_t kNetworkObjectEndpointStateOffset = 4;
constexpr uint32_t kNetworkObjectAuthoritativeOffset = 14;
constexpr uint32_t kNetworkObjectOwnerPeerOffset = 15;
constexpr uint32_t kNetworkObjectPendingOwnerOffset = 16;
constexpr uint32_t kNetworkObjectFlagsOffset = 17;
constexpr uint32_t kNetworkObjectSyncFlagsOffset = 18;
constexpr uint32_t kNetworkObjectOwnershipTokenOffset = 160;
constexpr uint32_t kNetworkObjectPeerFlagsOffset = 40;
constexpr uint32_t kNetworkObjectPeerFlagsStride = 3;
constexpr uint32_t kNetworkEndpointSyncCurrentOffset = 8;
constexpr uint32_t kNetworkEndpointSyncQueuedOffset = 9;
constexpr uint32_t kNetworkEndpointSyncModeOffset = 10;
constexpr uint32_t kNetworkEndpointSyncTimerOffset = 12;
constexpr uint32_t kNetworkObjectCreateEndpointVtableOffset = 68;
constexpr uint32_t kNetworkObjectRecipientFilterVtableOffset = 172;
constexpr uint32_t kNetworkObjectRecipientModeVtableOffset = 232;
constexpr uint32_t kNetworkObjectPeerJoinedVtableOffset = 128;
constexpr uint32_t kNetworkObjectPeerDepartedVtableOffset = 132;
constexpr uint32_t kNetworkObjectRecipientAllowedVtableOffset = 92;
constexpr uint32_t kNetworkObjectExpiredVtableOffset = 76;
constexpr uint32_t kNetworkObjectPrepareSyncVtableOffset = 140;
constexpr uint32_t kNetworkObjectBlenderFactoryVtableOffset = 64;
constexpr uint32_t kNetworkObjectOwnershipEligibleVtableOffset = 72;
constexpr uint32_t kNetworkObjectComponentCountVtableOffset = 196;
constexpr uint32_t kNetworkObjectFillOwnershipCommandVtableOffset = 236;
constexpr uint32_t kNetworkObjectCreateSyncVtableOffset = 224;
constexpr uint32_t kNetworkObjectSerializeSyncVtableOffset = 156;
constexpr uint32_t kNetworkPeerMaskLegacyBits = 16;
constexpr uint32_t kNetworkPeerMaskHeaderBits = 17;
constexpr uint32_t kNetworkPeerMaskExtensionChunkBits = 16;
constexpr size_t kNetworkPeerMaskExtensionChunkCount = 3;
constexpr uint32_t kNetworkPeerMaskExtensionMarker = 0x10000;
constexpr uint32_t kNetworkPeerMaskLegacyMask = 0xFFFF;
constexpr uint32_t kNetworkPeerMaskApplyReturnAddress = 0x82707C30;
constexpr uint32_t kEventPeerOutboundBufferSize = 1040;
constexpr uint32_t kEventPeerInboundBufferSize = 2080;
constexpr uint32_t kEventPeerOutboundTableOffset = 4592;
constexpr uint32_t kEventPeerInboundTableOffset = 21236;
constexpr uint32_t kEventScopeMaskOffset = 8;
constexpr uint16_t kEventScopeHighPeerSentinel = 0x8000;
constexpr uint32_t kEventPeerManagerPointerOffset = 8;
constexpr uint32_t kEventListHeadOffset = 40;
constexpr uint32_t kEventNodeValueOffset = 4;
constexpr uint32_t kEventNodeNextOffset = 8;
constexpr uint32_t kEventMetadataPointerOffset = 12;
constexpr uint32_t kEventPeerScopeVtableOffset = 16;
constexpr size_t kEventQueueCapacity = 128;
constexpr uint32_t kEventEligiblePeerType = 5;
constexpr uint32_t kConnectionBroadcastPayloadSize = 8;
constexpr uint32_t kConnectionMessagePayloadPointerOffset = 12;
constexpr uint32_t kConnectionMessageAddressOffset = 32;
constexpr uint32_t kObjectManagerPeerManagerOffset = 304;
constexpr uint32_t kPlayerTickDisabledAddress = 0x82C30C14;
constexpr uint32_t kPlayerTickGlobalBase = 0x82A957F0;
constexpr uint32_t kPlayerTickGlobalActivityAddress = 0x82A957F4;
constexpr uint32_t kPlayerTickDeltaAddress = 0x82C74EB4;
constexpr uint32_t kPlayerTickScaleAddress = 0x82018A3C;
constexpr uint32_t kPlayerTickFirstByteArrayOffset = 8;
constexpr uint32_t kPlayerTickSecondByteArrayOffset = 24;
constexpr uint32_t kPlayerTickEightByteTableOffset = 40;
constexpr uint32_t kPlayerTickFourByteTableOffset = 168;
constexpr uint32_t kPlayerTickPointerTableOffset = 232;
constexpr uint32_t kPlayerTickLargeRecordOffset = 128448;
constexpr uint32_t kPlayerTickLargeRecordSize = 40;
constexpr uint32_t kPlayerTickMinimumInterval = 120;
constexpr uint32_t kPlayerTickSecondPassCapacity = 10;
constexpr size_t kProximityPeerResultCapacity = 32;
constexpr uint32_t kProximityPeerScratchSize = 256;
constexpr uint32_t kProximityPeerComparatorAddress = 0x826E4970;
constexpr uint32_t kProximityWeightLocalTransformOffset = 32;
constexpr uint32_t kProximityWeightPositionOffset = 48;
constexpr uint32_t kProximityWeightPeerPointerOffset = 1376;
constexpr uint32_t kProximityWeightStackOffset = 128;
constexpr uint32_t kProximityWeightFirstThresholdAddress = 0x82055F8C;
constexpr uint32_t kProximityWeightSecondThresholdAddress = 0x82055F30;
constexpr uint32_t kProximityWeightFourthThresholdAddress = 0x82055F7C;
constexpr uint32_t kProximityWeightFirstSlopeAddress = 0x820560EC;
constexpr uint32_t kProximityWeightSecondSlopeAddress = 0x82015190;
constexpr uint32_t kProximityWeightFourthSlopeAddress = 0x820BEE88;
constexpr uint32_t kNetworkClockAddress = 0x82C74E9C;
constexpr uint32_t kObjectUpdateCurrentTimeAddress = 0x82C74EC0;
constexpr uint32_t kObjectUpdateFlagsAddress = 0x8318D984;
constexpr uint32_t kObjectUpdatePreviousTimeAddress = 0x8318D980;
constexpr uint32_t kObjectManagerShortPeerTimeoutOffset = 333988;
constexpr uint32_t kObjectManagerLongPeerTimeoutOffset = 333992;
constexpr uint32_t kObjectManagerChannelTimeout = 3000;
constexpr size_t kObjectManagerChannelCount = 4;
constexpr uint32_t kObjectPeerSyncAckSize = 1048;
constexpr uint32_t kObjectPeerReliableSize = 4160;
constexpr uint32_t kObjectPeerQueueSize = 12;
constexpr uint32_t kObjectPeerMessageSize = 1032;
constexpr uint32_t kObjectPeerMessageTableOffset = 1468;
constexpr uint32_t kObjectPeerMessageQueueOffset = 8;
constexpr uint32_t kObjectPeerMessagePayloadOffset = 36;
constexpr uint32_t kObjectPeerMessagePayloadCapacity = 995;
constexpr uint32_t kObjectPeerSyncAckTableOffset = 17984;
constexpr uint32_t kObjectPeerReliableTableOffset = 34756;
constexpr uint32_t kObjectPeerQueueTableOffset = 101320;
constexpr uint32_t kObjectPeerQueueCountOffset = 8;
constexpr uint32_t kObjectPeerSequenceStride = 2;
constexpr uint32_t kObjectPeerSequenceTableOffset = 101520;
constexpr uint32_t kObjectOwnerListBias = 42;
constexpr uint32_t kObjectOwnerListHeaderSize = 8;
constexpr uint32_t kObjectOwnerListTableOffset = 336;
constexpr uint32_t kObjectOwnerListNodeObjectOffset = 4;
constexpr uint32_t kObjectOwnerListNodeNextOffset = 8;
constexpr uint32_t kObjectOwnerListNodePreviousOffset = 12;
constexpr uint32_t kObjectPeerMatrixBias = 32212;
constexpr uint32_t kObjectPeerMatrixLegacyStride = 16;
constexpr uint32_t kObjectPeerMatrixTableOffset = 128848;
constexpr uint32_t kObjectPeerMatrixLegacyRowSize = 64;
constexpr uint32_t kObjectPeerMatrixObjectCapacity = 3200;
constexpr uint32_t kObjectPeerMatrixLegacyBytes = 204800;
constexpr uint32_t kObjectPeerMatrixThresholdOffset = 333968;
constexpr uint32_t kObjectManagerInitializedFlagOffset = 334015;
constexpr uint32_t kObjectManagerReassignmentOffset = 108248;
constexpr size_t kObjectRemovalBatchCapacity = 200;
// These are the guest addresses of GTA's global pool-pointer slots, not the
// pool objects themselves. Extended endpoints use the retail descriptor's
// element stride while keeping the fixed pool itself at its retail capacity.
constexpr uint32_t kVehicleSyncPoolPointerAddress = 0x8318E7D4;
constexpr uint32_t kPlayerSyncPoolPointerAddress = 0x8318E7E8;
constexpr uint32_t kDummyPedSyncPoolPointerAddress = 0x8318E890;
constexpr uint32_t kPedSyncPoolPointerAddress = 0x8318E574;
constexpr uint32_t kObjectSyncPoolPointerAddress = 0x8318E7E4;
constexpr uint32_t kFixedPoolElementStrideOffset = 12;
constexpr uint32_t kReassignmentCommandRecordStride = 20;
constexpr uint32_t kReassignmentCommandRecordSize = 24;
constexpr uint32_t kReassignmentObjectListBias = 42;
constexpr uint32_t kReassignmentObjectListHeaderSize = 8;
constexpr uint32_t kReassignmentTransportStateSize = 76;
constexpr uint32_t kReassignmentTransportConstructorAddress = 0x829F0A70;
constexpr uint32_t kReassignmentTransportDestructorAddress = 0x829F0580;
constexpr uint32_t kReassignmentMessageParserStateAddress = 0x8318EDC4;
constexpr uint32_t kReassignmentStatusParserStateAddress = 0x8318EE04;
constexpr uint32_t kReassignmentMessageScratchSize = 64;
constexpr uint8_t kReassignmentOwnerAliasPeerId = 15;
constexpr uint8_t kReassignmentRecipientAliasPeerId = 0;

enum class PeerIdClass : uint8_t {
  kLegacy,
  kExtended,
  kInvalid,
};

constexpr PeerIdClass ClassifyPeerId(uint8_t peer_id) noexcept {
  if (peer_id < kLegacyPeerCapacity) {
    return PeerIdClass::kLegacy;
  }
  if (peer_id < kExtendedPeerCapacity) {
    return PeerIdClass::kExtended;
  }
  return PeerIdClass::kInvalid;
}

constexpr bool IsValidPeerId(uint8_t peer_id) noexcept {
  return ClassifyPeerId(peer_id) != PeerIdClass::kInvalid;
}

constexpr bool IsLegacyPeerId(uint8_t peer_id) noexcept {
  return ClassifyPeerId(peer_id) == PeerIdClass::kLegacy;
}

inline float LinearProximityWeight(float distance, float threshold, float slope) noexcept {
  if (distance <= threshold) {
    return 1.0f;
  }
  return std::max(1.0f - (distance - threshold) * slope, 0.0f);
}

inline std::array<uint32_t, 3> ComputeGlobalProximityStatusWeights(
    const std::array<uint32_t, 3>& category_counts, uint32_t random_value,
    const std::array<float, 3>& multipliers) noexcept {
  const uint32_t denominator =
      category_counts[0] +
      category_counts[1] * kProximityStatusCategoryOneDenominatorWeight +
      category_counts[2] * kProximityStatusCategoryTwoDenominatorWeight;
  if (denominator == 0) {
    return {};
  }
  const float share =
      static_cast<float>(random_value) / static_cast<float>(denominator);
  std::array<uint32_t, 3> weights{};
  for (size_t category = 0; category < weights.size(); ++category) {
    weights[category] = static_cast<uint32_t>(share * multipliers[category]);
  }
  return weights;
}

// Return an embedded guest-table offset only for a retail-safe peer ID. This
// is the only policy helper hooks should use before indexing manager memory.
constexpr std::optional<uint32_t> LegacyPeerPointerOffset(uint8_t peer_id) noexcept {
  if (!IsLegacyPeerId(peer_id)) {
    return std::nullopt;
  }
  return kLegacyPeerPointerTableOffset + static_cast<uint32_t>(peer_id) * kGuestPointerSize;
}

constexpr std::optional<uint32_t> LegacyPlayerInfoPointerAddress(uint8_t peer_id) noexcept {
  if (!IsLegacyPeerId(peer_id)) {
    return std::nullopt;
  }
  return kLegacyPlayerInfoPointerTableAddress + static_cast<uint32_t>(peer_id) * kGuestPointerSize;
}

constexpr std::optional<uint32_t> LegacyPlayerInfoGenerationAddress(uint8_t peer_id) noexcept {
  if (!IsLegacyPeerId(peer_id)) {
    return std::nullopt;
  }
  return kLegacyPlayerInfoGenerationTableAddress +
         static_cast<uint32_t>(peer_id) * kGuestPointerSize;
}

constexpr std::optional<uint32_t> CheckedGuestAddress(uint32_t guest_base,
                                                      uint64_t offset) noexcept {
  const uint64_t address = static_cast<uint64_t>(guest_base) + offset;
  if (address > std::numeric_limits<uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(address);
}

// Pointer-relative accesses must reject a null object before applying a field
// offset. CheckedGuestAddress intentionally accepts address zero for general
// guest-address arithmetic, so pointer walks use this stricter helper.
constexpr std::optional<uint32_t> CheckedGuestPointerAddress(uint32_t guest_pointer,
                                                             uint64_t offset) noexcept {
  if (guest_pointer == 0) {
    return std::nullopt;
  }
  return CheckedGuestAddress(guest_pointer, offset);
}

constexpr std::optional<uint32_t> CheckedGuestArrayAddress(uint32_t guest_base, size_t index,
                                                           uint32_t stride) noexcept {
  const uint64_t offset = static_cast<uint64_t>(index) * stride;
  return CheckedGuestAddress(guest_base, offset);
}

enum class CapacityPath : uint8_t {
  kLegacy,
  kExtended,
  kInvalid,
};

struct CapacityDecision {
  CapacityPath path = CapacityPath::kInvalid;
  uint32_t total = 0;
};

// Addition is widened before comparison, preventing a wrapped public/private
// slot sum from passing the validation gate.
constexpr CapacityDecision ClassifySlotRequest(uint32_t public_slots,
                                               uint32_t private_slots) noexcept {
  const uint64_t total = static_cast<uint64_t>(public_slots) + private_slots;
  if (total > kExtendedPeerCapacity) {
    return {};
  }
  if (total <= kLegacyCommandParticipantCapacity) {
    return {CapacityPath::kLegacy, static_cast<uint32_t>(total)};
  }
  return {CapacityPath::kExtended, static_cast<uint32_t>(total)};
}

struct FreeRoamSlotRequest {
  bool expanded = false;
  uint32_t public_slots = 0;
  uint32_t private_slots = 0;
};

constexpr FreeRoamSlotRequest ExpandFreeRoamSlotRequest(
    uint32_t game_mode, uint32_t public_slots, uint32_t private_slots) noexcept {
  const CapacityDecision decision = ClassifySlotRequest(public_slots, private_slots);
  if (game_mode != kFreeRoamGameMode || decision.path != CapacityPath::kLegacy ||
      private_slots > kExtendedPeerCapacity) {
    return {.public_slots = public_slots, .private_slots = private_slots};
  }
  return {.expanded = true,
          .public_slots = static_cast<uint32_t>(kExtendedPeerCapacity) - private_slots,
          .private_slots = private_slots};
}

constexpr CapacityDecision ClassifyParticipantCount(uint32_t count) noexcept {
  if (count > kExtendedPeerCapacity) {
    return {};
  }
  if (count <= kLegacyCommandParticipantCapacity) {
    return {CapacityPath::kLegacy, count};
  }
  return {CapacityPath::kExtended, count};
}

constexpr uint32_t LowBits32(uint32_t count) noexcept {
  if (count == 0) {
    return 0;
  }
  if (count >= std::numeric_limits<uint32_t>::digits) {
    return std::numeric_limits<uint32_t>::max();
  }
  return (uint32_t{1} << count) - 1;
}

struct LegacySlotProxy {
  bool valid = false;
  uint32_t public_slots = 0;
  uint32_t private_slots = 0;
};

// The retail SetMaxSlots routine can still perform its state checks,
// allocation, callback wiring and queue insertion when supplied a <=32 proxy.
// Its command is patched at the queue seam before any consumer observes it.
constexpr LegacySlotProxy MakeLegacySlotProxy(uint32_t public_slots,
                                              uint32_t private_slots) noexcept {
  const CapacityDecision decision = ClassifySlotRequest(public_slots, private_slots);
  if (decision.path == CapacityPath::kInvalid) {
    return {};
  }
  if (decision.path == CapacityPath::kLegacy) {
    return {true, public_slots, private_slots};
  }
  const uint32_t proxy_public = public_slots < kLegacyCommandParticipantCapacity
                                    ? public_slots
                                    : kLegacyCommandParticipantCapacity;
  return {true, proxy_public, kLegacyCommandParticipantCapacity - proxy_public};
}

class PeerMask64 {
 public:
  constexpr bool Set(uint8_t peer_id) noexcept {
    if (!IsValidPeerId(peer_id)) {
      return false;
    }
    bits_ |= uint64_t{1} << peer_id;
    return true;
  }

  constexpr bool Reset(uint8_t peer_id) noexcept {
    if (!IsValidPeerId(peer_id)) {
      return false;
    }
    bits_ &= ~(uint64_t{1} << peer_id);
    return true;
  }

  constexpr bool Contains(uint8_t peer_id) const noexcept {
    return IsValidPeerId(peer_id) && (bits_ & (uint64_t{1} << peer_id)) != 0;
  }

  constexpr uint64_t bits() const noexcept { return bits_; }
  constexpr uint16_t legacy_low16() const noexcept { return static_cast<uint16_t>(bits_); }
  constexpr uint32_t legacy_low32() const noexcept { return static_cast<uint32_t>(bits_); }
  constexpr bool has_nonlegacy16_bits() const noexcept {
    return (bits_ >> kLegacyPeerCapacity) != 0;
  }
  constexpr bool has_nonlegacy32_bits() const noexcept {
    return (bits_ >> kLegacyCommandParticipantCapacity) != 0;
  }
  constexpr size_t Count() const noexcept { return std::popcount(bits_); }

  constexpr void Clear() noexcept { bits_ = 0; }

  constexpr void ReplaceLegacyLow32(uint32_t low_bits) noexcept {
    constexpr uint64_t kLow32Mask = std::numeric_limits<uint32_t>::max();
    bits_ = (bits_ & ~kLow32Mask) | low_bits;
  }

  constexpr void ReplaceLegacyLow16(uint16_t low_bits) noexcept {
    constexpr uint64_t kLow16Mask = std::numeric_limits<uint16_t>::max();
    bits_ = (bits_ & ~kLow16Mask) | low_bits;
  }

  constexpr void ClearNonLegacy16() noexcept {
    constexpr uint64_t kLow16Mask = std::numeric_limits<uint16_t>::max();
    bits_ &= kLow16Mask;
  }

  constexpr void ReplaceAll(uint64_t bits) noexcept { bits_ = bits; }

 private:
  uint64_t bits_ = 0;
};

struct NetworkPeerMaskWireWords {
  uint32_t header = 0;
  std::array<uint16_t, kNetworkPeerMaskExtensionChunkCount> extension{};
  bool extended = false;
};

constexpr NetworkPeerMaskWireWords EncodeNetworkPeerMask(PeerMask64 mask) noexcept {
  NetworkPeerMaskWireWords wire;
  const uint64_t bits = mask.bits();
  wire.header = static_cast<uint32_t>(bits) & kNetworkPeerMaskLegacyMask;
  wire.extended = mask.has_nonlegacy16_bits();
  if (wire.extended) {
    wire.header |= kNetworkPeerMaskExtensionMarker;
    for (size_t chunk = 0; chunk < wire.extension.size(); ++chunk) {
      const size_t shift = kNetworkPeerMaskLegacyBits + chunk * kNetworkPeerMaskExtensionChunkBits;
      wire.extension[chunk] = static_cast<uint16_t>(bits >> shift);
    }
  }
  return wire;
}

constexpr PeerMask64 DecodeNetworkPeerMask(
    uint32_t header,
    const std::array<uint16_t, kNetworkPeerMaskExtensionChunkCount>& extension) noexcept {
  uint64_t bits = header & kNetworkPeerMaskLegacyMask;
  if ((header & kNetworkPeerMaskExtensionMarker) != 0) {
    for (size_t chunk = 0; chunk < extension.size(); ++chunk) {
      const size_t shift = kNetworkPeerMaskLegacyBits + chunk * kNetworkPeerMaskExtensionChunkBits;
      bits |= static_cast<uint64_t>(extension[chunk]) << shift;
    }
  }
  PeerMask64 mask;
  mask.ReplaceAll(bits);
  return mask;
}

struct PeerMaskPair64 {
  PeerMask64 first;
  PeerMask64 second;
};

struct ReassignmentOwnerState {
  std::array<uint8_t, kReassignmentCommandRecordSize> command_record{};
  std::array<uint8_t, kReassignmentObjectListHeaderSize> object_list{};
  std::array<uint32_t, kExtendedPeerCapacity> guest_transport_states{};
  PeerMask64 involved;
  PeerMask64 confirmed;
  PeerMask64 sent;
  bool initialized = false;
};

class ReassignmentRegistry {
 public:
  ReassignmentOwnerState Get(uint32_t guest_manager, uint8_t owner_id) const {
    if (guest_manager == 0 || !IsValidPeerId(owner_id)) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    return it == managers_.end() ? ReassignmentOwnerState{} : it->second[owner_id];
  }

  bool Set(uint32_t guest_manager, uint8_t owner_id, const ReassignmentOwnerState& state) {
    if (guest_manager == 0 || !IsValidPeerId(owner_id)) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    managers_[guest_manager][owner_id] = state;
    return true;
  }

  uint32_t GetTransport(uint32_t guest_manager, uint8_t owner_id, uint8_t recipient_id) const {
    if (!IsValidPeerId(recipient_id)) {
      return 0;
    }
    return Get(guest_manager, owner_id).guest_transport_states[recipient_id];
  }

  bool SetTransport(uint32_t guest_manager, uint8_t owner_id, uint8_t recipient_id,
                    uint32_t guest_transport) {
    if (guest_manager == 0 || !IsValidPeerId(owner_id) || !IsValidPeerId(recipient_id) ||
        guest_transport == 0) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    ReassignmentOwnerState& state = managers_[guest_manager][owner_id];
    state.guest_transport_states[recipient_id] = guest_transport;
    state.initialized = true;
    return true;
  }

  std::array<ReassignmentOwnerState, kExtendedPeerCapacity> RemoveManager(uint32_t guest_manager) {
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    if (it == managers_.end()) {
      return {};
    }
    const auto states = it->second;
    managers_.erase(it);
    return states;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::array<ReassignmentOwnerState, kExtendedPeerCapacity>> managers_;
};

class PeerMaskPairRegistry {
 public:
  bool Set(uint32_t guest_owner, uint8_t peer_id, bool first, bool second) {
    if (guest_owner == 0 || !IsValidPeerId(peer_id)) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    PeerMaskPair64& masks = entries_[guest_owner];
    return (!first || masks.first.Set(peer_id)) && (!second || masks.second.Set(peer_id));
  }

  bool Reset(uint32_t guest_owner, uint8_t peer_id, bool first, bool second) {
    if (guest_owner == 0 || !IsValidPeerId(peer_id)) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    PeerMaskPair64& masks = entries_[guest_owner];
    return (!first || masks.first.Reset(peer_id)) && (!second || masks.second.Reset(peer_id));
  }

  void ReplaceLegacyLow32(uint32_t guest_owner, uint32_t first, uint32_t second) {
    if (guest_owner == 0) {
      return;
    }
    std::scoped_lock lock(mutex_);
    PeerMaskPair64& masks = entries_[guest_owner];
    masks.first.ReplaceLegacyLow32(first);
    masks.second.ReplaceLegacyLow32(second);
  }

  void ReplaceLegacyLow16(uint32_t guest_owner, uint16_t first, uint16_t second) {
    if (guest_owner == 0) {
      return;
    }
    std::scoped_lock lock(mutex_);
    PeerMaskPair64& masks = entries_[guest_owner];
    masks.first.ReplaceLegacyLow16(first);
    masks.second.ReplaceLegacyLow16(second);
  }

  void ClearNonLegacy16(uint32_t guest_owner, bool first, bool second) {
    if (guest_owner == 0) {
      return;
    }
    std::scoped_lock lock(mutex_);
    PeerMaskPair64& masks = entries_[guest_owner];
    if (first) {
      masks.first.ClearNonLegacy16();
    }
    if (second) {
      masks.second.ClearNonLegacy16();
    }
  }

  void Clear(uint32_t guest_owner, bool first, bool second) {
    if (guest_owner == 0) {
      return;
    }
    std::scoped_lock lock(mutex_);
    PeerMaskPair64& masks = entries_[guest_owner];
    if (first) {
      masks.first.Clear();
    }
    if (second) {
      masks.second.Clear();
    }
  }

  PeerMaskPair64 Get(uint32_t guest_owner) const {
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(guest_owner);
    return it == entries_.end() ? PeerMaskPair64{} : it->second;
  }

  void Remove(uint32_t guest_owner) {
    std::scoped_lock lock(mutex_);
    entries_.erase(guest_owner);
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, PeerMaskPair64> entries_;
};

struct DispatchPeerState {
  std::vector<std::array<uint8_t, kDispatchElementStateRecordSize>> element_states;
  std::vector<uint8_t> pending_elements;
};

// Each dispatch element owns a retail 16-entry array of four-byte per-peer
// sequence state plus a 16-bit acknowledgement mask. Extended peers are kept
// here and projected through one legacy slot only for the duration of an
// original serializer/acknowledger call.
class DispatchPeerStateRegistry {
 public:
  DispatchPeerState Get(uint32_t guest_handler, uint8_t peer_id, size_t element_count) {
    if (guest_handler == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    DispatchPeerState& state = entries_[guest_handler][peer_id];
    state.element_states.resize(element_count);
    state.pending_elements.resize(element_count);
    return state;
  }

  bool Set(uint32_t guest_handler, uint8_t peer_id, const DispatchPeerState& state) {
    if (guest_handler == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended ||
        state.element_states.size() != state.pending_elements.size()) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    entries_[guest_handler][peer_id] = state;
    return true;
  }

  void RemovePeer(uint32_t guest_handler, uint8_t peer_id) {
    if (guest_handler == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return;
    }
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(guest_handler);
    if (it == entries_.end()) {
      return;
    }
    it->second[peer_id] = {};
  }

  void Reset(uint32_t guest_handler) {
    if (guest_handler == 0) {
      return;
    }
    std::scoped_lock lock(mutex_);
    entries_.erase(guest_handler);
  }

  void ResetElement(uint32_t guest_handler, size_t element) {
    if (guest_handler == 0) {
      return;
    }
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(guest_handler);
    if (it == entries_.end()) {
      return;
    }
    for (uint8_t peer_id = kLegacyPeerCapacity; peer_id < kExtendedPeerCapacity; ++peer_id) {
      DispatchPeerState& state = it->second[peer_id];
      if (element >= state.element_states.size() || element >= state.pending_elements.size()) {
        continue;
      }
      state.element_states[element] = {};
      state.pending_elements[element] = 0;
    }
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::array<DispatchPeerState, kExtendedPeerCapacity>> entries_;
};

template <typename T>
class ParticipantQueue64 {
 public:
  constexpr bool Push(const T& participant) noexcept {
    if (size_ == entries_.size()) {
      return false;
    }
    entries_[size_] = participant;
    ++size_;
    return true;
  }

  constexpr size_t size() const noexcept { return size_; }
  constexpr size_t capacity() const noexcept { return entries_.size(); }
  constexpr bool empty() const noexcept { return size_ == 0; }
  constexpr const T& operator[](size_t index) const noexcept { return entries_[index]; }

 private:
  std::array<T, kExtendedPeerCapacity> entries_{};
  size_t size_ = 0;
};

struct SessionParticipantState {
  uint32_t guest_record_table = 0;
  uint32_t count = 0;
  uint32_t public_count = 0;
  uint32_t private_count = 0;
};

// The retail session object places its count field exactly where participant
// 32 would begin. This registry owns the relocated 64-record guest table and
// keeps the title-visible aggregate counts without ever indexing through the
// overlapping inline region.
class SessionParticipantRegistry {
 public:
  bool Register(uint32_t guest_session, uint32_t guest_record_table) {
    if (guest_session == 0 || guest_record_table == 0) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    sessions_.insert_or_assign(guest_session,
                               SessionParticipantState{.guest_record_table = guest_record_table});
    return true;
  }

  SessionParticipantState Get(uint32_t guest_session) const {
    std::scoped_lock lock(mutex_);
    const auto it = sessions_.find(guest_session);
    return it == sessions_.end() ? SessionParticipantState{} : it->second;
  }

  bool SetCounts(uint32_t guest_session, uint32_t count, uint32_t public_count,
                 uint32_t private_count) {
    const uint64_t classified_count = static_cast<uint64_t>(public_count) + private_count;
    if (count > kExtendedPeerCapacity || public_count > count || private_count > count ||
        classified_count != count) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    const auto it = sessions_.find(guest_session);
    if (it == sessions_.end()) {
      return false;
    }
    it->second.count = count;
    it->second.public_count = public_count;
    it->second.private_count = private_count;
    return true;
  }

  bool Add(uint32_t guest_session, bool is_private) {
    std::scoped_lock lock(mutex_);
    const auto it = sessions_.find(guest_session);
    if (it == sessions_.end() || it->second.count == kExtendedPeerCapacity) {
      return false;
    }
    ++it->second.count;
    if (is_private) {
      ++it->second.private_count;
    } else {
      ++it->second.public_count;
    }
    return true;
  }

  bool Remove(uint32_t guest_session, bool was_private) {
    std::scoped_lock lock(mutex_);
    const auto it = sessions_.find(guest_session);
    if (it == sessions_.end() || it->second.count == 0 ||
        (was_private ? it->second.private_count : it->second.public_count) == 0) {
      return false;
    }
    --it->second.count;
    if (was_private) {
      --it->second.private_count;
    } else {
      --it->second.public_count;
    }
    return true;
  }

  SessionParticipantState RemoveSession(uint32_t guest_session) {
    std::scoped_lock lock(mutex_);
    const auto it = sessions_.find(guest_session);
    if (it == sessions_.end()) {
      return {};
    }
    const SessionParticipantState state = it->second;
    sessions_.erase(it);
    return state;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, SessionParticipantState> sessions_;
};

struct ParticipantCommandState {
  uint32_t guest_record_table = 0;
  uint32_t public_count = 0;
  uint32_t private_count = 0;

  constexpr uint32_t count() const noexcept { return public_count + private_count; }
};

class ParticipantCommandRegistry {
 public:
  bool Set(uint32_t guest_command, ParticipantCommandState state) {
    const uint64_t count = static_cast<uint64_t>(state.public_count) + state.private_count;
    if (guest_command == 0 || state.guest_record_table == 0 || count > kExtendedPeerCapacity) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    commands_.insert_or_assign(guest_command, state);
    return true;
  }

  ParticipantCommandState Get(uint32_t guest_command) const {
    std::scoped_lock lock(mutex_);
    const auto it = commands_.find(guest_command);
    return it == commands_.end() ? ParticipantCommandState{} : it->second;
  }

  ParticipantCommandState Remove(uint32_t guest_command) {
    std::scoped_lock lock(mutex_);
    const auto it = commands_.find(guest_command);
    if (it == commands_.end()) {
      return {};
    }
    const ParticipantCommandState state = it->second;
    commands_.erase(it);
    return state;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, ParticipantCommandState> commands_;
};

struct MigrationTaskState {
  uint32_t guest_record_table = 0;
  uint32_t count = 0;
  int32_t current = -1;
};

class MigrationTaskRegistry {
 public:
  bool Set(uint32_t guest_task, MigrationTaskState state) {
    if (guest_task == 0 || state.guest_record_table == 0 || state.count == 0 ||
        state.count > kExtendedPeerCapacity || state.current < -1 ||
        state.current > static_cast<int32_t>(state.count)) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    tasks_.insert_or_assign(guest_task, state);
    return true;
  }

  MigrationTaskState Get(uint32_t guest_task) const {
    std::scoped_lock lock(mutex_);
    const auto it = tasks_.find(guest_task);
    return it == tasks_.end() ? MigrationTaskState{} : it->second;
  }

  bool SetCurrent(uint32_t guest_task, int32_t current) {
    std::scoped_lock lock(mutex_);
    const auto it = tasks_.find(guest_task);
    if (it == tasks_.end() || current < -1 || current > static_cast<int32_t>(it->second.count)) {
      return false;
    }
    it->second.current = current;
    return true;
  }

  MigrationTaskState Remove(uint32_t guest_task) {
    std::scoped_lock lock(mutex_);
    const auto it = tasks_.find(guest_task);
    if (it == tasks_.end()) {
      return {};
    }
    const MigrationTaskState state = it->second;
    tasks_.erase(it);
    return state;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, MigrationTaskState> tasks_;
};

class RoundRobinCursor {
 public:
  constexpr size_t start(size_t population) noexcept {
    if (population == 0) {
      next_ = 0;
      return 0;
    }
    if (next_ >= population) {
      next_ = 0;
    }
    return next_;
  }

  constexpr void Advance(size_t population, size_t count) noexcept {
    if (population == 0) {
      next_ = 0;
      return;
    }
    next_ = (start(population) + (count % population)) % population;
  }

 private:
  size_t next_ = 0;
};

class PeerManagerSidecar {
 public:
  bool SetPeer(uint8_t peer_id, uint32_t guest_peer) noexcept {
    if (!IsValidPeerId(peer_id)) {
      return false;
    }
    peers_[peer_id] = guest_peer;
    return true;
  }

  uint32_t GetPeer(uint8_t peer_id) const noexcept {
    if (!IsValidPeerId(peer_id)) {
      return 0;
    }
    return peers_[peer_id];
  }

  bool RemovePeer(uint8_t peer_id) noexcept {
    if (!IsValidPeerId(peer_id)) {
      return false;
    }
    peers_[peer_id] = 0;
    return true;
  }

  size_t CountExtendedPeers() const noexcept {
    size_t count = 0;
    for (size_t peer_id = kLegacyPeerCapacity; peer_id < peers_.size(); ++peer_id) {
      if (peers_[peer_id] != 0) {
        ++count;
      }
    }
    return count;
  }

  template <typename Visitor>
  void VisitExtendedPeers(Visitor&& visitor) const {
    for (size_t peer_id = kLegacyPeerCapacity; peer_id < peers_.size(); ++peer_id) {
      if (peers_[peer_id] != 0 && !visitor(static_cast<uint8_t>(peer_id), peers_[peer_id])) {
        return;
      }
    }
  }

 private:
  std::array<uint32_t, kExtendedPeerCapacity> peers_{};
};

// Managers are embedded in title-owned guest objects, so their guest address
// is the stable lifetime key. The registry never writes beyond the retail
// manager object and can be discarded atomically during its destructor hook.
class PeerManagerRegistry {
 public:
  bool RegisterManager(uint32_t guest_manager) {
    if (guest_manager == 0) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    managers_.insert_or_assign(guest_manager, PeerManagerSidecar{});
    return true;
  }

  bool HasManager(uint32_t guest_manager) const {
    std::scoped_lock lock(mutex_);
    return managers_.contains(guest_manager);
  }

  void UnregisterManager(uint32_t guest_manager) {
    std::scoped_lock lock(mutex_);
    managers_.erase(guest_manager);
  }

  bool SetPeer(uint32_t guest_manager, uint8_t peer_id, uint32_t guest_peer) {
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    return it != managers_.end() && it->second.SetPeer(peer_id, guest_peer);
  }

  uint32_t GetPeer(uint32_t guest_manager, uint8_t peer_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    return it == managers_.end() ? 0 : it->second.GetPeer(peer_id);
  }

  bool RemovePeer(uint32_t guest_manager, uint8_t peer_id) {
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    return it != managers_.end() && it->second.RemovePeer(peer_id);
  }

  size_t CountExtendedPeers(uint32_t guest_manager) const {
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    return it == managers_.end() ? 0 : it->second.CountExtendedPeers();
  }

  template <typename Visitor>
  void VisitExtendedPeers(uint32_t guest_manager, Visitor&& visitor) const {
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    if (it != managers_.end()) {
      it->second.VisitExtendedPeers(static_cast<Visitor&&>(visitor));
    }
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, PeerManagerSidecar> managers_;
};

struct PlayerInfoEntry {
  uint32_t guest_player_info = 0;
  uint32_t generation = 0;
};

class PlayerInfoRegistry {
 public:
  bool Set(uint8_t peer_id, PlayerInfoEntry entry) {
    if (ClassifyPeerId(peer_id) != PeerIdClass::kExtended || entry.guest_player_info == 0) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    entries_[peer_id] = entry;
    return true;
  }

  PlayerInfoEntry Get(uint8_t peer_id) const {
    if (ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    return entries_[peer_id];
  }

  PlayerInfoEntry Remove(uint8_t peer_id) {
    if (ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    const PlayerInfoEntry entry = entries_[peer_id];
    entries_[peer_id] = {};
    return entry;
  }

  std::array<PlayerInfoEntry, kExtendedPeerCapacity> Snapshot() const {
    std::scoped_lock lock(mutex_);
    return entries_;
  }

  size_t Count() const {
    std::scoped_lock lock(mutex_);
    return static_cast<size_t>(std::count_if(
        entries_.begin() + kLegacyPeerCapacity, entries_.end(),
        [](const PlayerInfoEntry& entry) { return entry.guest_player_info != 0; }));
  }

 private:
  mutable std::mutex mutex_;
  std::array<PlayerInfoEntry, kExtendedPeerCapacity> entries_{};
};

class NetworkEndpointRegistry {
 public:
  bool Set(uint32_t guest_object, uint8_t peer_id, uint32_t guest_endpoint) {
    if (guest_object == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended ||
        guest_endpoint == 0) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    uint32_t& registered_endpoint = objects_[guest_object][peer_id];
    if (registered_endpoint != 0 && registered_endpoint != guest_endpoint) {
      return false;
    }
    registered_endpoint = guest_endpoint;
    return true;
  }

  uint32_t Get(uint32_t guest_object, uint8_t peer_id) const {
    if (ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return 0;
    }
    std::scoped_lock lock(mutex_);
    const auto it = objects_.find(guest_object);
    return it == objects_.end() ? 0 : it->second[peer_id];
  }

  uint32_t Remove(uint32_t guest_object, uint8_t peer_id) {
    if (ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return 0;
    }
    std::scoped_lock lock(mutex_);
    const auto it = objects_.find(guest_object);
    if (it == objects_.end()) {
      return 0;
    }
    const uint32_t endpoint = it->second[peer_id];
    it->second[peer_id] = 0;
    return endpoint;
  }

  template <typename Visitor>
  void VisitExtended(uint32_t guest_object, Visitor&& visitor) const {
    std::scoped_lock lock(mutex_);
    const auto it = objects_.find(guest_object);
    if (it == objects_.end()) {
      return;
    }
    for (size_t peer_id = kLegacyPeerCapacity; peer_id < it->second.size(); ++peer_id) {
      const uint32_t endpoint = it->second[peer_id];
      if (endpoint != 0 && !visitor(static_cast<uint8_t>(peer_id), endpoint)) {
        return;
      }
    }
  }

  void RemoveObject(uint32_t guest_object) {
    std::scoped_lock lock(mutex_);
    objects_.erase(guest_object);
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::array<uint32_t, kExtendedPeerCapacity>> objects_;
};

using NetworkObjectPeerFlags = std::array<uint8_t, kNetworkObjectPeerFlagsStride>;

using PedNetworkPeerState = std::array<uint8_t, kPedNetworkPeerStateStride>;

class PedNetworkPeerStateRegistry {
 public:
  PedNetworkPeerState Get(uint32_t guest_object, uint8_t peer_id) const {
    if (guest_object == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    const auto it = objects_.find(guest_object);
    return it == objects_.end() ? PedNetworkPeerState{} : it->second[peer_id];
  }

  bool Set(uint32_t guest_object, uint8_t peer_id, PedNetworkPeerState state) {
    if (guest_object == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    objects_[guest_object][peer_id] = state;
    return true;
  }

  void RemoveObject(uint32_t guest_object) {
    std::scoped_lock lock(mutex_);
    objects_.erase(guest_object);
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::array<PedNetworkPeerState, kExtendedPeerCapacity>> objects_;
};

class NetworkObjectPeerFlagsRegistry {
 public:
  NetworkObjectPeerFlags Get(uint32_t guest_object, uint8_t peer_id) const {
    if (guest_object == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    const auto it = objects_.find(guest_object);
    return it == objects_.end() ? NetworkObjectPeerFlags{} : it->second[peer_id];
  }

  bool Set(uint32_t guest_object, uint8_t peer_id, NetworkObjectPeerFlags flags) {
    if (guest_object == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    objects_[guest_object][peer_id] = flags;
    return true;
  }

  bool SetFlag(uint32_t guest_object, uint8_t peer_id, size_t flag_index, bool value) {
    if (guest_object == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended ||
        flag_index >= kNetworkObjectPeerFlagsStride) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    objects_[guest_object][peer_id][flag_index] = value ? 1 : 0;
    return true;
  }

  void ClearPeer(uint32_t guest_object, uint8_t peer_id) { Set(guest_object, peer_id, {}); }

  void RemoveObject(uint32_t guest_object) {
    std::scoped_lock lock(mutex_);
    objects_.erase(guest_object);
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::array<NetworkObjectPeerFlags, kExtendedPeerCapacity>> objects_;
};

struct EventPeerBuffers {
  uint32_t guest_outbound = 0;
  uint32_t guest_inbound = 0;
};

class EventPeerBufferRegistry {
 public:
  EventPeerBuffers Get(uint32_t guest_manager, uint8_t peer_id) const {
    if (guest_manager == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    return it == managers_.end() ? EventPeerBuffers{} : it->second[peer_id];
  }

  bool Set(uint32_t guest_manager, uint8_t peer_id, EventPeerBuffers buffers) {
    if (guest_manager == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended ||
        buffers.guest_outbound == 0 || buffers.guest_inbound == 0) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    managers_[guest_manager][peer_id] = buffers;
    return true;
  }

  EventPeerBuffers RemovePeer(uint32_t guest_manager, uint8_t peer_id) {
    if (guest_manager == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    if (it == managers_.end()) {
      return {};
    }
    const EventPeerBuffers buffers = it->second[peer_id];
    it->second[peer_id] = {};
    return buffers;
  }

  std::array<EventPeerBuffers, kExtendedPeerCapacity> RemoveManager(uint32_t guest_manager) {
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    if (it == managers_.end()) {
      return {};
    }
    const auto buffers = it->second;
    managers_.erase(it);
    return buffers;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::array<EventPeerBuffers, kExtendedPeerCapacity>> managers_;
};

class EventScopeRegistry {
 public:
  PeerMask64 Get(uint32_t guest_event) const {
    std::scoped_lock lock(mutex_);
    const auto it = events_.find(guest_event);
    return it == events_.end() ? PeerMask64{} : it->second;
  }

  bool Set(uint32_t guest_event, PeerMask64 scope) {
    if (guest_event == 0) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    events_[guest_event] = scope;
    return true;
  }

  void Remove(uint32_t guest_event) {
    std::scoped_lock lock(mutex_);
    events_.erase(guest_event);
  }

  void ResetPeer(uint8_t peer_id) {
    if (!IsValidPeerId(peer_id)) {
      return;
    }
    std::scoped_lock lock(mutex_);
    for (auto& [guest_event, scope] : events_) {
      (void)guest_event;
      scope.Reset(peer_id);
    }
  }

  std::vector<std::pair<uint32_t, PeerMask64>> Snapshot() const {
    std::scoped_lock lock(mutex_);
    std::vector<std::pair<uint32_t, PeerMask64>> result;
    result.reserve(events_.size());
    for (const auto& entry : events_) {
      result.push_back(entry);
    }
    return result;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, PeerMask64> events_;
};

struct ObjectManagerPeerTiming {
  uint32_t last_received = 0;
  std::array<uint32_t, kObjectManagerChannelCount> channel_received{};
};

struct PlayerTickState {
  uint8_t first_byte = 0;
  uint8_t second_byte = 0;
  std::array<uint8_t, 8> eight_byte_record{};
  std::array<uint8_t, 4> four_byte_record{};
  std::array<uint8_t, 4> pointer_record{};
  std::array<uint8_t, kPlayerTickLargeRecordSize> large_record{};

  constexpr bool operator==(const PlayerTickState&) const = default;
};

class PlayerTickStateRegistry {
 public:
  PlayerTickState Get(uint32_t guest_manager, uint8_t peer_id) const {
    if (guest_manager == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    return it == managers_.end() ? PlayerTickState{} : it->second[peer_id];
  }

  bool Set(uint32_t guest_manager, uint8_t peer_id, const PlayerTickState& state) {
    if (guest_manager == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    managers_[guest_manager][peer_id] = state;
    return true;
  }

  void RemovePeer(uint32_t guest_manager, uint8_t peer_id) {
    if (ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return;
    }
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    if (it != managers_.end()) {
      it->second[peer_id] = {};
    }
  }

  void RemoveManager(uint32_t guest_manager) {
    std::scoped_lock lock(mutex_);
    managers_.erase(guest_manager);
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::array<PlayerTickState, kExtendedPeerCapacity>> managers_;
};

class ObjectManagerPeerTimingRegistry {
 public:
  ObjectManagerPeerTiming Get(uint32_t guest_manager, uint8_t peer_id) const {
    if (guest_manager == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    return it == managers_.end() ? ObjectManagerPeerTiming{} : it->second[peer_id];
  }

  bool SetLastReceived(uint32_t guest_manager, uint8_t peer_id, uint32_t timestamp) {
    if (guest_manager == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    managers_[guest_manager][peer_id].last_received = timestamp;
    return true;
  }

  bool SetChannelReceived(uint32_t guest_manager, uint8_t peer_id, size_t channel,
                          uint32_t timestamp) {
    if (guest_manager == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended ||
        channel >= kObjectManagerChannelCount) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    managers_[guest_manager][peer_id].channel_received[channel] = timestamp;
    return true;
  }

  void RemovePeer(uint32_t guest_manager, uint8_t peer_id) {
    if (ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return;
    }
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    if (it != managers_.end()) {
      it->second[peer_id] = {};
    }
  }

  void RemoveManager(uint32_t guest_manager) {
    std::scoped_lock lock(mutex_);
    managers_.erase(guest_manager);
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::array<ObjectManagerPeerTiming, kExtendedPeerCapacity>>
      managers_;
};

struct ObjectManagerPeerBuffers {
  uint32_t guest_message = 0;
  uint32_t guest_sync_ack = 0;
  uint32_t guest_reliable = 0;
  uint32_t guest_queue = 0;
  uint16_t sequence = 0;
};

class ObjectManagerPeerBufferRegistry {
 public:
  ObjectManagerPeerBuffers Get(uint32_t guest_manager, uint8_t peer_id) const {
    if (guest_manager == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    return it == managers_.end() ? ObjectManagerPeerBuffers{} : it->second[peer_id];
  }

  bool Set(uint32_t guest_manager, uint8_t peer_id, ObjectManagerPeerBuffers buffers) {
    if (guest_manager == 0 || ClassifyPeerId(peer_id) != PeerIdClass::kExtended ||
        buffers.guest_message == 0 || buffers.guest_sync_ack == 0 || buffers.guest_reliable == 0 ||
        buffers.guest_queue == 0) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    managers_[guest_manager][peer_id] = buffers;
    return true;
  }

  ObjectManagerPeerBuffers RemovePeer(uint32_t guest_manager, uint8_t peer_id) {
    if (ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    if (it == managers_.end()) {
      return {};
    }
    const ObjectManagerPeerBuffers buffers = it->second[peer_id];
    it->second[peer_id] = {};
    return buffers;
  }

  std::array<ObjectManagerPeerBuffers, kExtendedPeerCapacity> RemoveManager(
      uint32_t guest_manager) {
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    if (it == managers_.end()) {
      return {};
    }
    const auto buffers = it->second;
    managers_.erase(it);
    return buffers;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::array<ObjectManagerPeerBuffers, kExtendedPeerCapacity>>
      managers_;
};

struct ObjectOwnerList {
  uint32_t guest_head = 0;
  uint32_t guest_tail = 0;

  bool empty() const noexcept { return guest_head == 0; }
};

class ObjectOwnerListRegistry {
 public:
  ObjectOwnerList Get(uint32_t guest_manager, uint8_t owner_id) const {
    if (guest_manager == 0 || ClassifyPeerId(owner_id) != PeerIdClass::kExtended) {
      return {};
    }
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    return it == managers_.end() ? ObjectOwnerList{} : it->second[owner_id];
  }

  bool Set(uint32_t guest_manager, uint8_t owner_id, ObjectOwnerList list) {
    if (guest_manager == 0 || ClassifyPeerId(owner_id) != PeerIdClass::kExtended) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    managers_[guest_manager][owner_id] = list;
    return true;
  }

  void ClearOwner(uint32_t guest_manager, uint8_t owner_id) {
    if (ClassifyPeerId(owner_id) != PeerIdClass::kExtended) {
      return;
    }
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    if (it != managers_.end()) {
      it->second[owner_id] = {};
    }
  }

  std::array<ObjectOwnerList, kExtendedPeerCapacity> RemoveManager(uint32_t guest_manager) {
    std::scoped_lock lock(mutex_);
    const auto it = managers_.find(guest_manager);
    if (it == managers_.end()) {
      return {};
    }
    const auto lists = it->second;
    managers_.erase(it);
    return lists;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::array<ObjectOwnerList, kExtendedPeerCapacity>> managers_;
};

class ObjectPeerMatrixRegistry {
 public:
  uint32_t Get(uint32_t guest_manager, uint16_t object_id, uint8_t peer_id) const {
    if (!IsValidKey(guest_manager, object_id, peer_id)) {
      return 0;
    }
    std::scoped_lock lock(mutex_);
    const auto manager = managers_.find(guest_manager);
    if (manager == managers_.end()) {
      return 0;
    }
    const auto object = manager->second.find(object_id);
    return object == manager->second.end() ? 0 : object->second[peer_id];
  }

  bool Set(uint32_t guest_manager, uint16_t object_id, uint8_t peer_id, uint32_t value) {
    if (!IsValidKey(guest_manager, object_id, peer_id)) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    auto& objects = managers_[guest_manager];
    if (value == 0) {
      const auto object = objects.find(object_id);
      if (object != objects.end()) {
        object->second[peer_id] = 0;
        if (std::all_of(object->second.begin() + kLegacyPeerCapacity, object->second.end(),
                        [](uint32_t entry) { return entry == 0; })) {
          objects.erase(object);
        }
      }
      return true;
    }
    objects[object_id][peer_id] = value;
    return true;
  }

  uint32_t Increment(uint32_t guest_manager, uint16_t object_id, uint8_t peer_id) {
    if (!IsValidKey(guest_manager, object_id, peer_id)) {
      return 0;
    }
    std::scoped_lock lock(mutex_);
    return ++managers_[guest_manager][object_id][peer_id];
  }

  void ClearObject(uint32_t guest_manager, uint16_t object_id) {
    std::scoped_lock lock(mutex_);
    const auto manager = managers_.find(guest_manager);
    if (manager != managers_.end()) {
      manager->second.erase(object_id);
    }
  }

  void ClearPeer(uint32_t guest_manager, uint8_t peer_id) {
    if (ClassifyPeerId(peer_id) != PeerIdClass::kExtended) {
      return;
    }
    std::scoped_lock lock(mutex_);
    const auto manager = managers_.find(guest_manager);
    if (manager == managers_.end()) {
      return;
    }
    for (auto object = manager->second.begin(); object != manager->second.end();) {
      object->second[peer_id] = 0;
      if (std::all_of(object->second.begin() + kLegacyPeerCapacity, object->second.end(),
                      [](uint32_t entry) { return entry == 0; })) {
        object = manager->second.erase(object);
      } else {
        ++object;
      }
    }
  }

  void RemoveManager(uint32_t guest_manager) {
    std::scoped_lock lock(mutex_);
    managers_.erase(guest_manager);
  }

 private:
  static bool IsValidKey(uint32_t guest_manager, uint16_t object_id, uint8_t peer_id) {
    return guest_manager != 0 && object_id != 0 && object_id < kObjectPeerMatrixObjectCapacity &&
           ClassifyPeerId(peer_id) == PeerIdClass::kExtended;
  }

  using ObjectRows = std::unordered_map<uint16_t, std::array<uint32_t, kExtendedPeerCapacity>>;
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, ObjectRows> managers_;
};

static_assert(LegacyPeerPointerOffset(0).value() == 1452);
static_assert(LegacyPeerPointerOffset(15).value() == 1512);
static_assert(!LegacyPeerPointerOffset(16).has_value());
static_assert(LegacyPlayerInfoPointerAddress(15).value() == 0x82B61E2C);
static_assert(LegacyPlayerInfoGenerationAddress(15).value() == 0x82B61DEC);
static_assert(!LegacyPlayerInfoPointerAddress(16).has_value());
static_assert(!LegacyPlayerInfoGenerationAddress(16).has_value());
static_assert(kExtendedPlayerInfoShadowTableSize ==
              static_cast<uint32_t>(kExtendedPeerCapacity) * kGuestPointerSize);
static_assert(kLegacyPeerPointerTableOffset +
                  static_cast<uint32_t>(kLegacyPeerCapacity) * kGuestPointerSize ==
              kPeerManagerCapacityFieldOffset);
static_assert(kLegacyNetworkEndpointTableOffset +
                  static_cast<uint32_t>(kLegacyPeerCapacity) * kGuestPointerSize ==
              kFirstOverlappingNetworkEndpointOffset);

}  // namespace gta4::multiplayer64

#endif  // GTA4_MULTIPLAYER_64_POLICY_H_
