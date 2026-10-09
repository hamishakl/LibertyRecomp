#include <catch2/catch_test_macros.hpp>
#include <rex/ui/frame_pacer.h>
#include <rex/ui/paint_wakeup_state.h>
#include <rex/ui/presentation_clock.h>
#include <array>
#include <future>
#include <thread>
#include <vector>
using namespace rex::ui;
using namespace std::chrono_literals;

TEST_CASE("Single pacer preserves rational cadence at every supported rate", "[frame-pacer]") {
  for (uint32_t fps : {30u,40u,60u,120u}) {
    FramePacer p;
    const uint64_t origin = 1'000'000'000;
    p.Configure(fps, origin);
    uint64_t now = origin;
    for (uint64_t frame = 0; frame < 1200; ++frame) {
      auto plan = p.Plan(now);
      if (plan.delay_ns) { now += plan.delay_ns; plan = p.Plan(now); }
      REQUIRE(plan.slot_ns == origin + frame * FramePacer::kSecond / fps);
      REQUIRE(plan.delay_ns == 0);
      REQUIRE(plan.display_target_ns == origin + (frame+1) * FramePacer::kSecond / fps);
      p.Queued(now);
    }
  }
}
TEST_CASE("Retries do not spend a slot and long stalls do not release a burst", "[frame-pacer]") {
  FramePacer p; p.Configure(40, 1'000'000'000);
  const auto initial = p.Plan(1'000'000'000);
  REQUIRE(p.Plan(1'001'000'000).slot_ns == initial.slot_ns);
  p.Queued(1'001'000'000);
  REQUIRE(p.Plan(1'002'000'000).delay_ns == 23'000'000);
  auto late = p.Plan(2'000'000'000);
  REQUIRE(late.delay_ns == 0);
  p.Queued(2'000'000'000);
  REQUIRE(p.Plan(2'000'000'001).delay_ns == 24'999'999);
}
TEST_CASE("Cap changes, unlocked, clock regression, and surface reset drop stale deadlines", "[frame-pacer]") {
  FramePacer p;
  for (auto fps : {30u,40u,60u,120u,0u,30u}) {
    const auto previous = p.generation();
    p.Configure(fps, 2'000'000'000);
    const auto a = p.Plan(2'000'000'000);
    REQUIRE(a.delay_ns == 0);
    REQUIRE(a.fps == fps);
    REQUIRE(a.generation > previous);
    p.Queued(2'000'000'000);
  }
  REQUIRE(p.Plan(1).delay_ns == 0);
  p.Reset(); REQUIRE(p.Plan(2).delay_ns == 0);
  p.Configure(1001,3); REQUIRE(p.fps() == 0);
}
TEST_CASE("Validated display phase is adopted only once per rate and surface epoch", "[frame-pacer]") {
  FramePacer p;p.Configure(40,1'000'000'000);p.Plan(1'000'000'000);p.Queued(1'001'000'000);
  REQUIRE_FALSE(p.ObservePhase(1'050'000'000,1'040'000'000));
  REQUIRE(p.ObservePhase(1'008'000'000,1'015'000'000));
  REQUIRE(p.Plan(1'015'000'000).slot_ns == 1'033'000'000);
  REQUIRE_FALSE(p.ObservePhase(1'009'000'000,1'015'000'000));
  p.Configure(60,1'040'000'000);REQUIRE_FALSE(p.phase_observed());
}
TEST_CASE("One display timeline absorbs wake and paint cost variation within the budget", "[frame-pacer]") {
  FramePacer p;p.Configure(40,1'000'000'000);
  p.Plan(1'000'000'000);p.Queued(1'001'000'000);
  REQUIRE(p.ObservePhase(1'005'000'000,1'010'000'000));
  uint64_t now = 1'010'000'000, previous_target = 0;
  for (unsigned i=0;i<600;++i) {
    auto plan=p.Plan(now);
    now += plan.delay_ns + (i%2 ? 5'000'000 : 0);
    plan=p.Plan(now);
    const uint64_t work = i%2 ? 9'000'000 : 1'000'000;
    REQUIRE(now+work < plan.display_target_ns);
    if(previous_target) REQUIRE(plan.display_target_ns-previous_target == 25'000'000);
    previous_target=plan.display_target_ns;
    p.Queued(now+work);now+=work;
  }
}
TEST_CASE("Deadline arithmetic saturates safely near the clock boundary", "[frame-pacer]") {
  FramePacer p;p.Configure(30,UINT64_MAX-10);auto a=p.Plan(UINT64_MAX-10);
  REQUIRE(a.display_target_ns==UINT64_MAX);p.Queued(UINT64_MAX-5);
  REQUIRE(p.Plan(UINT64_MAX-4).delay_ns==4);
  REQUIRE_FALSE(p.ObservePhase(1,UINT64_MAX-4));
}
TEST_CASE("Publication acknowledgement blocks only the producer and consumes the exact serial", "[frame-pacer]") {
  FramePublicationGate g;g.SetAvailable(true);
  auto first=g.Publish(40);g.Accept(first);
  REQUIRE(g.Wait(first));
  auto second=g.Publish(40);
  auto wait=std::async(std::launch::async,[&]{return g.Wait(second);});
  REQUIRE(wait.wait_for(5ms)==std::future_status::timeout);
  g.Accept(first);
  REQUIRE(wait.wait_for(5ms)==std::future_status::timeout);
  g.Accept(second+1);
  REQUIRE(wait.wait_for(5ms)==std::future_status::timeout);
  g.Accept(second); REQUIRE(wait.wait_for(100ms)==std::future_status::ready);REQUIRE(wait.get());
}
TEST_CASE("Producer handoff cancels on surface loss, shutdown, and unlocked mode", "[frame-pacer]") {
  for(int action=0;action<3;++action) {
    FramePublicationGate g;g.SetAvailable(true);auto serial=g.Publish(40);
    auto wait=std::async(std::launch::async,[&]{return g.Wait(serial);});
    REQUIRE(wait.wait_for(5ms)==std::future_status::timeout);
    if(action==0)g.SetAvailable(false);else if(action==1)g.Stop();else g.Publish(0);
    REQUIRE(wait.wait_for(100ms)==std::future_status::ready);REQUIRE(wait.get());
  }
}
TEST_CASE("Producer watchdog scales with the frame limit and never drops below 50 ms", "[frame-pacer]") {
  REQUIRE(FramePublicationGate::WatchdogNs(60)==50'000'000);
  REQUIRE(FramePublicationGate::WatchdogNs(120)==50'000'000);
  REQUIRE(FramePublicationGate::WatchdogNs(30)==100'000'000);
  REQUIRE(FramePublicationGate::WatchdogNs(10)==250'000'000);
  REQUIRE(FramePublicationGate::WatchdogNs(0)==250'000'000);
  // An unadmitted publication (hidden window) releases after the watchdog, not 250 ms.
  FramePublicationGate g;g.SetAvailable(true);auto serial=g.Publish(60);
  const auto begin=std::chrono::steady_clock::now();
  REQUIRE_FALSE(g.Wait(serial));
  const auto waited=std::chrono::steady_clock::now()-begin;
  REQUIRE(waited>=45ms);REQUIRE(waited<150ms);
}
TEST_CASE("Paint wake tickets preempt delayed timers without consuming newer work", "[frame-pacer]") {
  PaintWakeupState s;
  auto deferred=s.Request(1000);REQUIRE(deferred!=0);
  REQUIRE(s.Request(2000)==0);
  auto immediate=s.Request(100);REQUIRE(immediate!=0);
  REQUIRE_FALSE(s.IsCurrent(deferred));REQUIRE_FALSE(s.Complete(deferred));
  REQUIRE(s.IsCurrent(immediate));REQUIRE(s.Complete(immediate));
  auto newer=s.Request(300);REQUIRE_FALSE(s.Complete(immediate));REQUIRE(s.IsCurrent(newer));
  s.Stop();REQUIRE_FALSE(s.IsCurrent(newer));REQUIRE(s.Request(1)==0);
  PaintWakeupState reopened;auto fresh=reopened.Request(1);
  REQUIRE(fresh!=newer);REQUIRE_FALSE(reopened.Complete(newer));
}
TEST_CASE("Clock mapping correlates epochs and rejects unsafe measurements", "[frame-pacer]") {
  PresentationClockMapping m;
  REQUIRE(m.ToDriver(100)==0);
  REQUIRE_FALSE(m.Sample(100,0,101));REQUIRE_FALSE(m.Sample(101,100,100));
  REQUIRE_FALSE(m.Sample(100,100,300101));
  REQUIRE(m.Sample(1000000,5000000,1000200));
  REQUIRE(m.uncertainty()==100);REQUIRE(m.ToDriver(1000100)==5000000);
  REQUIRE(m.ToDriver(2000100)==6000000);REQUIRE(m.ToHost(6000000)==2000100);
  REQUIRE(m.ToHost(1)==0);
}
TEST_CASE("Feedback accepts only matching issued IDs, epoch data and plausible times", "[frame-pacer]") {
  PresentationFeedbackHistory h;
  auto id=h.NewID();h.Insert({id,12,40,3,100,200,7});
  auto good=h.Take(id,200,210,250);REQUIRE(good);REQUIRE(good->serial==7);
  REQUIRE_FALSE(h.Take(id,200,210,250));
  id=h.NewID();h.Insert({id,12,40,3,100,200,7});REQUIRE_FALSE(h.Take(id,201,210,250));
  id=h.NewID();h.Insert({id,12,40,3,100,200,7});REQUIRE_FALSE(h.Take(id,200,99,250));
  id=h.NewID();h.Insert({id,12,40,3,100,200,7});REQUIRE_FALSE(h.Take(id,200,4'000'000,250));
  id=h.NewID();h.Insert({id,12,40,3,100,200,7});h.Clear();REQUIRE_FALSE(h.Take(id,200,210,250));
  id=h.NewID();h.Insert({id,12,40,3,100,200,7});
  REQUIRE_FALSE(h.Take(id,200,210,3'000'000'000));
}

#include "../../../src/ui/metal/display_timing.h"

TEST_CASE("Display-linked admission follows predicted display instead of callback arrival", "[frame-pacer][metal]") {
  for (uint32_t hz : {60u, 120u}) for (uint32_t fps : {30u, 40u, 60u, 120u}) {
    FramePacer pacer;
    const uint64_t origin = 10 * FramePacer::kSecond;
    pacer.Configure(fps, origin);
    size_t accepted = 0;
    uint64_t previous_target = 0;
    for (uint64_t tick = 0; tick < hz * 4; ++tick) {
      const uint64_t target = origin + tick * FramePacer::kSecond / hz;
      const uint64_t now = target - 6'000'000 + (tick % 2 ? 200'000 : 0);
      const auto plan = pacer.PlanForDisplay(now, target);
      if (plan.delay_ns) continue;
      CHECK(plan.display_target_ns == target);
      CHECK(target > previous_target);
      CHECK(pacer.PlanForDisplay(now, target).slot_ns == plan.slot_ns);
      pacer.Queued(now + 1'000'000);
      previous_target = target; ++accepted;
    }
    CHECK(accepted == std::min(fps, hz) * 4);
  }
}

TEST_CASE("Display pacing does not consume retries or carry deadlines across mode changes", "[frame-pacer][metal]") {
  FramePacer pacer; const uint64_t origin = 10 * FramePacer::kSecond;
  pacer.Configure(40, origin);
  auto a = pacer.PlanForDisplay(origin, origin + 5'000'000);
  REQUIRE(a.delay_ns == 0);
  REQUIRE(pacer.PlanForDisplay(origin + 1, origin + 5'000'000).slot_ns == a.slot_ns);
  pacer.Queued(origin + 2'000'000);
  REQUIRE(pacer.PlanForDisplay(origin + 10'000'000, origin + 15'000'000).delay_ns > 0);
  auto late = pacer.PlanForDisplay(origin + FramePacer::kSecond, origin + FramePacer::kSecond + 5'000'000);
  REQUIRE(late.delay_ns == 0);
  pacer.Queued(origin + FramePacer::kSecond + 1'000'000);
  const auto generation = pacer.generation();
  REQUIRE(pacer.Plan(origin + FramePacer::kSecond + 2'000'000).delay_ns == 0);
  REQUIRE(pacer.generation() > generation);
  REQUIRE(pacer.PlanForDisplay(origin + FramePacer::kSecond + 3'000'000, 0).delay_ns > 0);
}

TEST_CASE("Metal timestamp feedback is bounded and rejects stale surface or cap epochs", "[frame-pacer][metal]") {
  using namespace rex::ui::metal;
  CHECK(MetalTimeNanoseconds(1.0) == FramePacer::kSecond);
  CHECK(MetalTimeNanoseconds(-1.0) == 0);
  CHECK(MetalTimeNanoseconds(std::numeric_limits<double>::infinity()) == 0);
  CHECK(MetalTimeNanoseconds(std::numeric_limits<double>::quiet_NaN()) == 0);
  DisplayFeedback value{1, 2, 3, 100, 200, 210, 4, 40};
  REQUIRE(IsCurrentDisplayFeedback(value, 1, 2, 40, 250));
  REQUIRE_FALSE(IsCurrentDisplayFeedback(value, 2, 2, 40, 250));
  REQUIRE_FALSE(IsCurrentDisplayFeedback(value, 1, 3, 40, 250));
  REQUIRE_FALSE(IsCurrentDisplayFeedback(value, 1, 2, 60, 250));
  value.actual_host_ns = 0; REQUIRE_FALSE(IsCurrentDisplayFeedback(value, 1, 2, 40, 250));
  DisplayFeedbackInbox inbox;
  for (size_t i = 0; i < DisplayFeedbackInbox::kCapacity + 7; ++i) {
    value.publication = i; inbox.Push(value);
  }
  auto batch = inbox.Drain();
  REQUIRE(batch.count == DisplayFeedbackInbox::kCapacity);
  REQUIRE(batch.overwritten == 7);
  REQUIRE(batch.values.front().publication == 7);
  REQUIRE(inbox.Drain().count == 0);
}


#include "../../../src/ui/metal/drawable_request.h"
#include <atomic>
#include <memory>

TEST_CASE("Metal display activity survives normal publication gaps", "[frame-pacer][metal]") {
  using namespace rex::ui::metal;
  for (uint32_t fps : {17u, 30u, 40u, 60u, 120u}) {
    DisplayLinkActivity activity;
    uint64_t now = 10 * FramePacer::kSecond;
    for (uint64_t frame = 0; frame < 1000; ++frame) {
      activity.Demand(now);
      CHECK_FALSE(activity.ShouldPause(now + 1, false));
      now += FramePacer::kSecond / fps;
      CHECK_FALSE(activity.ShouldPause(now, false));
    }
    activity.Demand(now);
    CHECK_FALSE(activity.ShouldPause(now + DisplayLinkActivity::kIdleGraceNs - 1, false));
    CHECK(activity.ShouldPause(now + DisplayLinkActivity::kIdleGraceNs, false));
    CHECK_FALSE(activity.ShouldPause(now + FramePacer::kSecond, true));
    CHECK_FALSE(activity.ShouldPause(now + FramePacer::kSecond + 1, false));
    CHECK_FALSE(activity.ShouldPause(1, false));
  }
}
TEST_CASE("Metal drawable completions cannot consume another request", "[frame-pacer][metal]") {
  using namespace rex::ui::metal;
  DrawableRequest<std::shared_ptr<int>> requests;
  auto first = requests.Begin(); REQUIRE(bool(first)); REQUIRE_FALSE(bool(requests.Begin()));
  auto pixel = std::make_shared<int>(1); std::weak_ptr<int> retained = pixel;
  REQUIRE(requests.Complete(first, std::move(pixel))); REQUIRE_FALSE(bool(requests.Begin()));
  auto output = requests.Take(); REQUIRE((output && *output == 1)); output.reset();
  CHECK(retained.expired());
  auto second = requests.Begin(); REQUIRE(bool(second));
  CHECK_FALSE(requests.Complete(first, std::make_shared<int>(99))); CHECK(requests.pending());
  requests.Reset(); CHECK(requests.pending()); REQUIRE_FALSE(bool(requests.Begin()));
  CHECK_FALSE(requests.Complete(second, std::make_shared<int>(2))); CHECK_FALSE(requests.pending());
  CHECK_FALSE(requests.Take());
  auto third = requests.Begin(); REQUIRE(bool(third));
  REQUIRE(requests.Complete(third, {})); CHECK_FALSE(requests.Take());
  auto final = requests.Begin(); REQUIRE(bool(final)); requests.Reset(true);
  CHECK_FALSE(requests.Complete(final, std::make_shared<int>(3)));
  CHECK_FALSE(bool(requests.Begin())); CHECK_FALSE(requests.Take());
}
TEST_CASE("Metal drawable request reset races retain bounded ownership", "[frame-pacer][metal]") {
  using namespace rex::ui::metal;
  DrawableRequest<std::shared_ptr<int>> requests;
  for (int i = 0; i < 1000; ++i) {
    auto ticket = requests.Begin(); REQUIRE(bool(ticket));
    std::atomic<bool> start{false};
    std::thread worker([&]{
      start.store(true, std::memory_order_release);
      requests.Complete(ticket, std::make_shared<int>(i));
    });
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    requests.Reset(); worker.join();
    CHECK_FALSE(requests.pending()); CHECK_FALSE(requests.Take());
  }
}
TEST_CASE("Metal duplicate display callbacks never consume two frame slots", "[frame-pacer][metal]") {
  for (uint32_t fps : {0u, 30u, 40u, 60u, 120u}) {
    FramePacer pacer; const uint64_t origin = 10 * FramePacer::kSecond;
    pacer.Configure(fps, origin);
    uint64_t accepted = 0;
    for (uint64_t tick = 0; tick < 1200; ++tick) {
      const auto target = origin + tick * FramePacer::kSecond / 120;
      const auto now = target - 5'000'000;
      auto plan = pacer.PlanForDisplay(now, target);
      if (plan.delay_ns) continue;
      pacer.Queued(now); pacer.Queued(now); ++accepted;
      CHECK(pacer.PlanForDisplay(now, target).delay_ns != 0);
      CHECK(pacer.PlanForDisplay(now, target - 1).delay_ns != 0);
    }
    CHECK(accepted == uint64_t(fps ? fps : 120) * 10);
  }
}
TEST_CASE("Metal missing presentation time is distinct from stale feedback", "[frame-pacer][metal]") {
  using namespace rex::ui::metal;
  DisplayFeedback item{1, 2, 3, 100, 200, 0, 4, 40};
  CHECK(IsCurrentDisplayReceipt(item, 1, 2, 40, 250));
  CHECK_FALSE(IsCurrentDisplayFeedback(item, 1, 2, 40, 250));
  CHECK_FALSE(IsCurrentDisplayReceipt(item, 2, 2, 40, 250));
  CHECK_FALSE(IsCurrentDisplayReceipt(item, 1, 3, 40, 250));
  CHECK_FALSE(IsCurrentDisplayReceipt(item, 1, 2, 60, 250));
  CHECK_FALSE(IsCurrentDisplayReceipt(item, 1, 2, 40, 99));
  CHECK_FALSE(IsCurrentDisplayReceipt(item, 1, 2, 40, 3 * FramePacer::kSecond));
  CHECK(IsUsableDisplayOpportunity(100, 200, 150));
  CHECK_FALSE(IsUsableDisplayOpportunity(200, 200, 150));
  CHECK_FALSE(IsUsableDisplayOpportunity(100, 200, 0));
  CHECK_FALSE(IsUsableDisplayOpportunity(100, 200, 201));
}

TEST_CASE("Metal software wakeups never consume future or canceled deadlines", "[frame-pacer][metal]") {
  using namespace rex::ui::metal;
  SoftwarePaintDeadline pending;
  CHECK_FALSE(pending.Consume(100));
  REQUIRE(pending.Request(100, 200)); CHECK(pending.due() == 300);
  CHECK_FALSE(pending.Request(100, 300)); CHECK(pending.due() == 300);
  CHECK_FALSE(pending.Consume(299)); CHECK(pending.due() == 300);
  REQUIRE(pending.Request(100, 50)); CHECK(pending.due() == 150);
  CHECK(pending.Consume(150)); CHECK_FALSE(pending.Consume(300));
  REQUIRE(pending.Request(200, 100)); pending.Reset(); CHECK_FALSE(pending.Consume(400));
  REQUIRE(pending.Request(200, 500)); CHECK_FALSE(pending.Consume(400));
  CHECK(pending.Consume(700));
  REQUIRE(pending.Request(800, 0)); CHECK(pending.due() == 801);
  pending.Reset(); REQUIRE(pending.Request(UINT64_MAX - 2, UINT64_MAX));
  CHECK(pending.due() == UINT64_MAX); CHECK(pending.Consume(UINT64_MAX));
}
