#pragma once

#import <Metal/Metal.h>

#include <array>
#include <cstddef>
#include <string>

#include "upload_arena.h"

namespace rex::ui::metal {

// Single CPU owner. Metal's command-buffer status is the GPU completion boundary.
// No completion block captures a renderer, window, or frame pointer.
class FrameRing {
 public:
  // Up to three title command buffers in flight: with two, CPU submit and GPU time both just
  // under a 60 Hz tick still miss ticks on jitter (alternating 17/33 ms frames). The active
  // count comes from gta4_native_frames_in_flight.
  static constexpr size_t kSlotCount = 3;
  struct Slot {
    explicit Slot(size_t budget = UploadArena::kDefaultBudget) : uploads(budget) {}
    UploadArena uploads;
    id<MTLCommandBuffer> submission = nil;
    bool encoding = false;
  };
  explicit FrameRing(size_t upload_budget = UploadArena::kDefaultBudget, size_t slot_count = kSlotCount)
      : slots_{Slot{upload_budget}, Slot{upload_budget}, Slot{upload_budget}},
        active_slot_count_(slot_count < 1 ? 1 : slot_count > kSlotCount ? kSlotCount : slot_count) {}
  FrameRing(const FrameRing&) = delete;
  FrameRing& operator=(const FrameRing&) = delete;
  ~FrameRing();

  Slot* TryBegin(std::string& error);
  Slot* Begin(std::string& error);
  bool Commit(Slot& slot, id<MTLCommandBuffer> command_buffer);
  void Cancel(Slot& slot);
  bool WaitIdle(std::string& error);
  size_t reserved_bytes() const;

 private:
  bool Owns(const Slot& slot) const;
  std::array<Slot, kSlotCount> slots_;
  size_t next_ = 0;
  size_t active_slot_count_ = kSlotCount;
};

}  // namespace rex::ui::metal
