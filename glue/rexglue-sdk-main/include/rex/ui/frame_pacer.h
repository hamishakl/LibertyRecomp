#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>

namespace rex::ui {

inline uint64_t FramePacerNowNs() noexcept {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One timeline, owned by the existing exclusive presenter paint owner. A slot
// is committed only by a successful host present, never by a retry or a game
// command being enqueued. No GPU lifetime or display-clock assumption lives here.
class FramePacer {
 public:
  static constexpr uint64_t kSecond = 1'000'000'000;
  struct Attempt {
    uint64_t slot_ns = 0;
    uint64_t delay_ns = 0;
    uint64_t display_target_ns = 0;  // Host clock; translate only after calibration.
    uint32_t fps = 0;
    uint64_t generation = 0;
  };

  static constexpr uint64_t Add(uint64_t a, uint64_t b) noexcept {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
  }
  void Configure(uint32_t fps, uint64_t now_ns) noexcept {
    if (fps > 1000) fps = 0;
    if (fps != fps_ || now_ns < last_observed_ns_) {
      Reset();
      fps_ = fps;
    }
    last_observed_ns_ = now_ns;
  }
  // Frame interpolation supplies an observed half-render interval. This is a
  // constraint on this timeline, not a second scheduler or presentation timer.
  void SetMinimumInterval(uint64_t ns) noexcept {
    minimum_interval_ns_=std::min(ns,kSecond/10);
    if(!minimum_interval_ns_&&!fps_){next_ns_=0;fraction_=0;}
  }
  uint64_t minimum_interval() const noexcept {return minimum_interval_ns_;}
  uint64_t period() const noexcept {
    return std::max(fps_?kSecond/fps_:uint64_t{0},minimum_interval_ns_);
  }
  void Reset() noexcept {
    next_ns_ = last_observed_ns_ = last_queued_display_target_ns_ = 0;
    fraction_ = 0;
    phase_observed_ = false;
    attempt_ = {};
    ++generation_;
  }
  Attempt Plan(uint64_t now_ns) noexcept {
    Configure(fps_, now_ns);
    if (display_driven_) { Reset(); display_driven_ = false; }
    const uint64_t period = this->period();
    if (!period) return attempt_ = {0, 0, 0, 0, generation_};
    if (!next_ns_ || (now_ns >= next_ns_ && now_ns - next_ns_ >= period)) {
      if (next_ns_) ++missed_slots_;
      next_ns_ = now_ns;
      fraction_ = 0;
    }
    // At most one interval of display lead. CPU admission and timed display
    // use this SAME slot, not two independently advancing clocks.
    attempt_ = {next_ns_, next_ns_ > now_ns ? next_ns_ - now_ns : 0,
                Add(next_ns_, period + (fps_ && minimum_interval_ns_ <= kSecond/fps_ && fraction_ + kSecond % fps_ >= fps_ ? 1 : 0)),
                fps_, generation_};
    return attempt_;
  }
  // A display-link callback is an opportunity, not another software timer.
  // Compare its predicted presentation time with the same rational timeline.
  // Queue time must not gate an opportunity that arrives before its display.
  Attempt PlanForDisplay(uint64_t now_ns, uint64_t target_ns) noexcept {
    Configure(fps_, now_ns);
    if (!display_driven_) { Reset(); display_driven_ = true; }
    if (!target_ns || target_ns <= last_queued_display_target_ns_ ||
        target_ns < now_ns || target_ns - now_ns > kSecond)
      return attempt_ = {0, 1, 0, fps_, generation_};
    const uint64_t period = this->period();
    if (!period) return attempt_ = {0, 0, target_ns, 0, generation_};
    if (!next_ns_ || (target_ns >= next_ns_ && target_ns - next_ns_ >= period)) {
      if (next_ns_) ++missed_slots_;
      next_ns_ = target_ns;
      fraction_ = 0;
    }
    // Bounded timestamp rounding/jitter allowance, never a whole display tick.
    const uint64_t tolerance = std::min(uint64_t{250'000}, period / 32);
    const uint64_t delay = next_ns_ > target_ns && next_ns_ - target_ns > tolerance
        ? next_ns_ - target_ns : 0;
    return attempt_ = {next_ns_, delay, target_ns, fps_, generation_};
  }
  void Queued(uint64_t queue_end_ns) noexcept {
    if (attempt_.generation != generation_ || attempt_.delay_ns) return;
    if (display_driven_) last_queued_display_target_ns_ = attempt_.display_target_ns;
    attempt_.generation = 0;  // A queue receipt can consume an attempt only once.
    const uint64_t whole = this->period();
    if (!whole) return;
    const bool rational_cap = fps_ && minimum_interval_ns_ <= kSecond/fps_;
    if(rational_cap)fraction_ += uint32_t(kSecond%fps_);else fraction_=0;
    const uint64_t period=whole+(rational_cap&&fraction_>=fps_?1:0);
    if(rational_cap)fraction_%=fps_;
    const uint64_t slot=attempt_.slot_ns?attempt_.slot_ns:
        display_driven_?attempt_.display_target_ns:queue_end_ns;
    next_ns_ = Add(slot, period);
    // Long stalls do not grant a burst of overdue frame slots.
    if (queue_end_ns >= next_ns_ && queue_end_ns - next_ns_ >= whole) {
      next_ns_ = Add(queue_end_ns, whole);
      fraction_ = 0;
      ++missed_slots_;
    }
  }
  // Called only with a validated, matched driver observation translated to the
  // host clock. Use it once per rate/surface epoch to establish the display phase.
  bool ObservePhase(uint64_t actual_host_ns, uint64_t now_ns) noexcept {
    if (!fps_ || display_driven_ || phase_observed_ || !actual_host_ns || actual_host_ns > now_ns ||
        now_ns - actual_host_ns > 2 * kSecond) return false;
    const uint64_t floor = std::max(next_ns_, now_ns);
    const uint64_t elapsed = floor - actual_host_ns;
    if (elapsed > 3 * kSecond) return false;
    const uint64_t periods = (elapsed / kSecond) * fps_ +
                            ((elapsed % kSecond) * fps_) / kSecond + 1;
    const uint64_t remainder = periods * (kSecond % fps_);
    next_ns_ = Add(actual_host_ns, periods * (kSecond / fps_) + remainder / fps_);
    fraction_ = uint32_t(remainder % fps_);
    phase_observed_ = true;
    return true;
  }
  uint32_t fps() const noexcept { return fps_; }
  uint64_t generation() const noexcept { return generation_; }
  uint64_t missed_slots() const noexcept { return missed_slots_; }
  bool phase_observed() const noexcept { return phase_observed_; }

 private:
  uint32_t fps_ = 0;
  uint32_t fraction_ = 0;
  uint64_t minimum_interval_ns_=0;
  uint64_t next_ns_ = 0;
  uint64_t last_observed_ns_ = 0;
  uint64_t last_queued_display_target_ns_ = 0;
  uint64_t generation_ = 1;
  uint64_t missed_slots_ = 0;
  bool phase_observed_ = false;
  bool display_driven_ = false;
  Attempt attempt_{};
};

// Back-pressure, NOT another timer/cadence. Only the native render worker opts
// into waiting. It has already submitted its GPU work and holds no presenter,
// queue, resource or UI mutex. The UI only acknowledges; it never waits here.
class FramePublicationGate {
 public:
  // `ahead` publications may be outstanding before the producer blocks (0 = the published
  // frame itself must be admitted first; 1 = the producer may run one frame ahead of the host,
  // trading up to one frame of latency for immunity to single-tick jitter).
  uint64_t Publish(uint32_t fps,bool paired=false,uint32_t ahead=0) {
    std::lock_guard lock(mutex_);
    limited_ = fps != 0 || paired;
    fps_ = fps;
    ahead_ = ahead;
    const auto result = ++published_;
    condition_.notify_all();
    return result;
  }
  void Admit(uint64_t serial) {
    std::lock_guard lock(mutex_);
    if(serial<=published_)admitted_=std::max(admitted_,serial);
    condition_.notify_all();
  }
  void Accept(uint64_t serial) {
    std::lock_guard lock(mutex_);
    if (serial <= published_) {accepted_ = std::max(accepted_, serial);admitted_=std::max(admitted_,serial);}
    condition_.notify_all();
  }
  bool Wait(uint64_t serial) {
    std::unique_lock lock(mutex_);
    const uint64_t epoch = epoch_;
    // A minimized/occluded window or missing callback must not hang the title.
    // This is a watchdog, never a frame-rate target. It also sets the title's pace
    // while the host is not presenting (window hidden, display link stopped):
    // three periods at the title's own limit keeps that at a third of the limit
    // (20 fps at 60) rather than 4 fps, while staying far above any foreground
    // handoff wait (measured p99 17 ms, max 61 ms at a 60 fps limit).
    const uint64_t watchdog = hidden_ ? kHiddenWatchdogNs : WatchdogNs(fps_);
    return condition_.wait_for(lock, std::chrono::nanoseconds(watchdog), [&] {
      return stopped_ || !available_ || !limited_ || epoch_ != epoch ||
             admitted_ + ahead_ >= serial;
    });
  }
  static constexpr uint64_t kHiddenWatchdogNs = 500'000'000;
  static constexpr uint64_t WatchdogNs(uint32_t fps) noexcept {
    constexpr uint64_t kFloor = 50'000'000, kCeiling = 250'000'000;
    return fps ? std::clamp<uint64_t>(3 * FramePacer::kSecond / fps, kFloor, kCeiling) : kCeiling;
  }
  void SetAvailable(bool value) {
    std::lock_guard lock(mutex_);
    available_ = value;
    if (!value) ++epoch_;
    condition_.notify_all();
  }
  // Occluded or minimized window: nobody can see the frames, so pace the title at the slow
  // watchdog (2 fps) instead of the three-period one, and wake it when the window shows again.
  void SetHidden(bool value) {
    std::lock_guard lock(mutex_);
    hidden_ = value;
    condition_.notify_all();
  }
  void Stop() {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    condition_.notify_all();
  }
 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  uint64_t published_ = 0, accepted_ = 0, admitted_=0, epoch_ = 0;
  uint32_t fps_ = 0, ahead_ = 0;
  bool limited_ = false, available_ = false, stopped_ = false, hidden_ = false;
};

}  // namespace rex::ui
