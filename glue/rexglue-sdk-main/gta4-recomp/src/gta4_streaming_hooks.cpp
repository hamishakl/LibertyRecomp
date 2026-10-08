#include "gta4_streaming_hooks.h"
#include "gta4_streaming_policy.h"
#include "gta4_streaming_trace_policy.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <memory>
#include <new>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/gta4_native/title_commands.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include "gta4_init.h"

REXCVAR_DEFINE_BOOL(gta4_streaming_modern, true, "GTA IV/Streaming",
                    "Enable bounded earlier world requests and deadline-based queue selection")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gta4_streaming_deadline_order, true, "GTA IV/Streaming",
                    "Select eligible requests by readiness deadline, keeping dependency and priority gates")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(gta4_streaming_budget_scale, 1.5, "GTA IV/Streaming",
                      "Scale nonzero requested budgets; retain original allocator capacity clamps")
    .range(1.0, 4.0).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(gta4_streaming_physical_reserve_mb, 16, "GTA IV/Streaming",
                      "Headroom retained for unmanaged resources when increasing the physical budget")
    .range(0, 256).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(gta4_streaming_preload_min, 100.0, "GTA IV/Streaming",
                      "Minimum preload-only margin in world units; does not move the fade boundary")
    .range(50.0, 500.0).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(gta4_streaming_preload_max, 250.0, "GTA IV/Streaming",
                      "Maximum movement-aware preload-only margin in world units")
    .range(50.0, 500.0).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(gta4_streaming_prefetch_pending_limit, 256, "GTA IV/Streaming",
                      "Stop extending speculative margins at this pending count; keep original requests")
    .range(16, 4096).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_streaming_trace_path, "", "GTA IV/Diagnostics",
                      "Optional bounded CSV streaming lifecycle trace; empty disables recording")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace gta4::streaming {
namespace {

// Verified in generated gta4_recomp.4/.33 and the parser's data references.
constexpr uint32_t kManager = 0x82A9AD00;
constexpr uint32_t kEntriesGlobal = 0x83084B14;
constexpr size_t kTraceCapacity = 32768;
constexpr uint64_t kTraceEventLimit = 65536;

uint64_t Now() noexcept {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
float Float(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(REX_LOAD_U32(address));
}
Vec3 Vector(uint8_t* base, uint32_t address) {
  return {Float(base, address), Float(base, address + 4), Float(base, address + 8)};
}
bool NativeMode() {
  auto* runtime = rex::Runtime::instance();
  auto* graphics = runtime ? runtime->graphics_system() : nullptr;
  return graphics && graphics->GetTitleCommandAbi(rex::graphics::gta4_native::kTitleId) ==
                         rex::graphics::gta4_native::kTitleCommandAbi;
}

struct Configuration {
  std::atomic<bool> modern{false};
  std::atomic<bool> deadline_order{false};
  std::atomic<double> budget_scale{1};
  std::atomic<uint32_t> physical_reserve_bytes{0};
  std::atomic<double> minimum{50};
  std::atomic<double> maximum{50};
  std::atomic<uint32_t> pending_limit{256};
};
// Published at the stream.ini boundary. Atomics also make diagnostic reads
// safe if an episode reinitializes configuration during asynchronous work.
Configuration config;
std::atomic<uint64_t> epoch{0};
std::atomic<uint32_t> bound_table{0};
DeadlineTable deadlines;
std::atomic<uint32_t> pending_count{0};
std::atomic<uint64_t> observed_world_latency{750000000};

struct WorldRequest {
  uint32_t entity = 0;
  uint32_t view = 0;
  uint32_t model = 0;
  uint64_t now = 0;
  uint64_t deadline = 0;
  double distance = 0;
  double threshold = 0;
};
thread_local WorldRequest world;
thread_local MotionTracker motion;
thread_local uint32_t last_view = 0;
thread_local uint16_t last_frame = 0;
thread_local uint64_t local_epoch = UINT64_MAX;
thread_local uint64_t frame_time = 0;
thread_local uint64_t last_log = 0;

struct Record {
  uint64_t generation = 0;
  uint64_t request_time = 0;
  uint64_t release_time = 0;
  uint64_t ready_time = 0;
  uint32_t table = 0;
  uint32_t entity = 0;
  uint32_t model = 0;
};
struct Event {
  uint64_t time = 0;
  uint64_t epoch = 0;
  uint64_t generation = 0;
  uint32_t entry = kInvalidEntry;
  uint32_t entity = 0;
  uint32_t model = 0;
  uint32_t kind = 0;
  uint32_t before = 0;
  uint32_t after = 0;
  uint32_t bytes = 0;
  uint64_t value = 0;
};
std::mutex records_mutex;
std::array<Record, kInvalidEntry> records{};
std::array<Event, kTraceCapacity> events{};
std::unique_ptr<ClassificationTraceCache> classification_cache;
size_t event_count = 0;
uint64_t dropped_events = 0;
uint64_t written_events = 0;
uint64_t reloads = 0;
uint64_t state_changes = 0;
uint64_t world_requests = 0;
std::atomic<bool> trace_enabled{false};
struct TraceFile {
  std::FILE* file = nullptr;
  bool attempted = false;
  ~TraceFile() { if (file) std::fclose(file); }
} trace;

void EventLocked(Event event) {
  if (!trace.file || written_events >= kTraceEventLimit) return;
  if (event_count == events.size()) { ++dropped_events; return; }
  event.epoch = epoch.load(std::memory_order_relaxed);
  events[event_count++] = event;
}

// Bind side data to the current guest table before publishing/consuming ranks.
// A replacement table can reuse every compact index; old deadlines must not
// silently survive that replacement. Only this rare transition scans the table.
void BindTable(uint32_t table) {
  if (bound_table.load(std::memory_order_acquire) == table) return;
  std::lock_guard lock(records_mutex);
  if (bound_table.load(std::memory_order_relaxed) == table) return;
  deadlines.Reset();
  records.fill({});
  if (classification_cache) classification_cache->Reset();
  pending_count.store(0, std::memory_order_relaxed);
  observed_world_latency.store(750000000, std::memory_order_relaxed);
  bound_table.store(table, std::memory_order_release);
  epoch.fetch_add(1, std::memory_order_release);
}

void Tick(uint8_t* base, uint32_t view) {
  if (!view || REX_LOAD_U8(view + 26) == 0) return;
  const uint64_t current_epoch = epoch.load(std::memory_order_acquire);
  if (local_epoch != current_epoch) {
    local_epoch = current_epoch;
    last_view = 0;
    frame_time = 0;
    last_log = 0;
    motion.Reset();
  }
  const uint16_t frame = REX_LOAD_U16(view + 56);
  if (frame_time && last_view == view && last_frame == frame) return;
  last_view = view;
  last_frame = frame;
  frame_time = Now();
  motion.Update(view, frame, frame_time, Vector(base, view + 2304));
  if (trace_enabled.load(std::memory_order_relaxed)) {
    FlushTrace();
    if (!last_log || frame_time - last_log >= 2000000000ULL) {
      last_log = frame_time;
      uint64_t changes, requests, cycles, dropped;
      {
        std::lock_guard lock(records_mutex);
        changes = state_changes; requests = world_requests;
        cycles = reloads; dropped = dropped_events;
      }
      REXLOG_INFO("gta4-streaming: epoch={} state-changes={} world-requests={} rerequests={} "
                  "pending={} observed-state1-ns={} trace-dropped={}",
                  current_epoch, changes, requests, cycles,
                  pending_count.load(std::memory_order_relaxed),
                  observed_world_latency.load(std::memory_order_relaxed), dropped);
      if (auto* kernel = rex::runtime::current_kernel_state()) kernel->LogHostIoStatistics();
    }
  }
}

struct WorldScope {
  WorldRequest previous = world;
  WorldScope(uint8_t* base, uint32_t entity, uint32_t view) {
    Tick(base, view);
    world = {};
    if (view && REX_LOAD_U8(view + 26) != 0) {
      world.entity = entity;
      world.view = view;
      world.now = frame_time;
      world.model = entity ? REX_LOAD_U16(entity + 46) : 0;
    }
  }
  ~WorldScope() { world = previous; }
};

// Only the operand of the final preload test in sub_821EE9E8 calls this.
// The existing caller consumes return code 3 via sub_82210EA0 -> sub_8259AAA8.
// No visible-distance, alpha, entity transform or script-owned state is changed.
float EntityPreloadMargin(uint8_t* base, uint32_t entity, uint32_t view,
                          double distance, double draw_distance, float retail_margin) {
  if (!world.entity || world.entity != entity || world.view != view) return retail_margin;
  uint32_t positioned = REX_LOAD_U32(entity + 76);
  if (!positioned) positioned = entity;
  const uint32_t matrix = REX_LOAD_U32(positioned + 32);
  const Vec3 position = Vector(base, matrix ? matrix + 48 : positioned + 16);
  const double closing = motion.ClosingSpeed(position);
  world.distance = distance;
  world.threshold = draw_distance;
  world.deadline = ReadinessDeadline(world.now, distance, draw_distance, closing, false);
  if (!config.modern || pending_count.load(std::memory_order_relaxed) >= config.pending_limit) {
    return retail_margin;
  }
  const double lead = std::clamp(
      static_cast<double>(observed_world_latency.load(std::memory_order_relaxed)) /
          kNanosecondsPerSecond, 0.75, 2.0);
  return PreloadMargin(retail_margin, config.minimum, config.maximum, closing, lead);
}

// The caller of this classifier requests models for results 2 and 3 and
// submits its render path for 1 and 2. Keep the raw codes in diagnostics;
// a classification is not proof that GPU pixels have been produced.
void ObserveClassification(uint8_t* base, uint32_t entity, uint32_t view,
                            double distance, uint32_t result) {
  if (!world.entity || world.entity != entity || world.view != view) return;
  if (result == 1 || result == 2) world.deadline = world.now;
  if (!trace_enabled.load(std::memory_order_relaxed)) return;
  uint32_t index = kInvalidEntry;
  uint32_t bytes = 0;
  // Same model->module-base addition as generated sub_82515870 and
  // sub_82210EA0. Reject the signed invalid model ID and module sentinel.
  const int16_t model_id = std::bit_cast<int16_t>(static_cast<uint16_t>(world.model));
  const uint32_t module = REX_LOAD_U32(0x82B58484);
  const uint32_t table = REX_LOAD_U32(kEntriesGlobal);
  if (model_id >= 0 && module < 0xFF && table && table == REX_LOAD_U32(kManager)) {
    BindTable(table);
    const uint32_t module_base = REX_LOAD_U32(0x82D59CB0 + module * 100 + 88);
    const uint64_t candidate = static_cast<uint64_t>(module_base) + model_id;
    const uint64_t address = static_cast<uint64_t>(table) + candidate * kEntryStride;
    if (candidate < kInvalidEntry && address + kEntryStride <= (uint64_t{1} << 32) &&
        REX_LOAD_U8(static_cast<uint32_t>(address) + 23) == module) {
      index = static_cast<uint32_t>(candidate);
      bytes = REX_LOAD_U32(static_cast<uint32_t>(address) + 8) & kSizeMask;
    }
  }
  std::lock_guard lock(records_mutex);
  if (!classification_cache || !trace.file || written_events >= kTraceEventLimit) return;
  // Do not advance the cache if the event cannot be recorded. Otherwise an
  // unchanged observation would never retry after the bounded buffer drains.
  if (event_count == events.size()) { ++dropped_events; return; }
  const uint64_t generation = index < kInvalidEntry ? records[index].generation : 0;
  const uint8_t alpha = REX_LOAD_U8(entity + 99);
  if (!classification_cache->ShouldEmit({generation, world.now, entity, view, world.model,
                                         index, static_cast<uint8_t>(result), alpha})) return;
  EventLocked({Now(), 0, generation, index, entity, world.model, 3,
               alpha, result, bytes, std::bit_cast<uint64_t>(distance)});
}

bool DeadlineOrderEnabled(uint8_t* base, uint32_t manager) {
  if (manager != kManager) return false;
  const uint32_t table = REX_LOAD_U32(kEntriesGlobal);
  if (!table || table != REX_LOAD_U32(manager)) return false;
  if (!config.modern || !config.deadline_order) return false;
  BindTable(table);
  pending_count.store(REX_LOAD_U32(manager + 56), std::memory_order_relaxed);
  return true;
}
uint64_t QueueRank(uint32_t index) { return deadlines.Rank(index); }
bool RankBefore(uint64_t rank, uint32_t sector, uint64_t best_rank, uint32_t best_sector) {
  return rank < best_rank || (rank == best_rank && sector < best_sector);
}

}  // namespace

static void FlushTraceLocked(size_t maximum_count) {
  if (!trace.file) return;
  const size_t count = std::min(event_count, maximum_count);
  for (size_t i = 0; i < count && written_events < kTraceEventLimit; ++i) {
    const Event& e = events[i];
    if (std::fprintf(trace.file, "%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%llu\n",
                     static_cast<unsigned long long>(e.time),
                     static_cast<unsigned long long>(e.epoch),
                     static_cast<unsigned long long>(e.generation), e.entry, e.entity, e.model,
                     e.kind, e.before, e.after, e.bytes,
                     static_cast<unsigned long long>(e.value)) < 0) {
      std::fclose(trace.file); trace.file = nullptr;
      trace_enabled.store(false, std::memory_order_relaxed);
      break;
    }
    ++written_events;
  }
  std::move(events.begin() + count, events.begin() + event_count, events.begin());
  event_count -= count;
  if (trace.file && std::fflush(trace.file) != 0) {
    REXLOG_WARN("gta4-streaming: trace flush failed; recording stopped");
    std::fclose(trace.file);
    trace.file = nullptr;
    trace_enabled.store(false, std::memory_order_relaxed);
  }
}

void FlushTrace() {
  // Bounded diagnostic batches, with no heap allocations in the producer path.
  std::lock_guard lock(records_mutex);
  FlushTraceLocked(512);
}

void FinishTrace() {
  std::lock_guard lock(records_mutex);
  trace_enabled.store(false, std::memory_order_relaxed);
  classification_cache.reset();
  if (!trace.file) { event_count = 0; return; }
  FlushTraceLocked(events.size());
  if (trace.file) {
    std::fclose(trace.file);
    trace.file = nullptr;
  }
  event_count = 0;
}

void Initialize(uint8_t* base) {
  (void)base;
  std::lock_guard lock(records_mutex);
  config.modern = NativeMode() && REXCVAR_GET(gta4_streaming_modern);
  config.deadline_order = REXCVAR_GET(gta4_streaming_deadline_order);
  config.budget_scale = REXCVAR_GET(gta4_streaming_budget_scale);
  config.physical_reserve_bytes = REXCVAR_GET(gta4_streaming_physical_reserve_mb) * (uint32_t{1} << 20);
  config.minimum = REXCVAR_GET(gta4_streaming_preload_min);
  config.maximum = REXCVAR_GET(gta4_streaming_preload_max);
  config.pending_limit = REXCVAR_GET(gta4_streaming_prefetch_pending_limit);
  deadlines.Reset();
  records.fill({});
  if (classification_cache) classification_cache->Reset();
  bound_table.store(0, std::memory_order_relaxed);
  pending_count.store(0, std::memory_order_relaxed);
  observed_world_latency.store(750000000, std::memory_order_relaxed);
  reloads = 0; state_changes = 0; world_requests = 0;
  epoch.fetch_add(1, std::memory_order_release);
  // One bounded file per process. An episode reset retains buffered events and
  // the open file, rather than failing exclusive creation on its own old path.
  if (!trace.attempted) {
    trace.attempted = true;
    const std::string path = REXCVAR_GET(gta4_streaming_trace_path);
    if (!path.empty()) {
      trace.file = std::fopen(path.c_str(), "wx");
      if (trace.file) {
        std::fputs("time_ns,epoch,generation,entry,entity,model,kind,before,after,bytes,value\n", trace.file);
      } else {
        REXLOG_WARN("gta4-streaming: could not create trace {}; recording disabled", path);
      }
    }
  }
  if (trace.file && !classification_cache) {
    classification_cache.reset(new (std::nothrow) ClassificationTraceCache());
    if (!classification_cache) {
      REXLOG_WARN("gta4-streaming: classification trace allocation failed; state tracing remains active");
    }
  }
  trace_enabled.store(trace.file != nullptr, std::memory_order_relaxed);
  REXLOG_INFO("gta4-streaming: modern={} deadline-order={} preload=[{},{}] budget-scale={} "
              "pending-limit={} trace={} physical-address-map=unchanged",
              config.modern.load(), config.deadline_order.load(), config.minimum.load(), config.maximum.load(),
              config.budget_scale.load(), config.pending_limit.load(), trace.file != nullptr);
}

}  // namespace gta4::streaming

using namespace gta4::streaming;

extern "C" void sub_82515870(PPCContext& ctx, uint8_t* base) {
  if (!config.modern && !trace_enabled.load(std::memory_order_relaxed)) {
    __imp__sub_82515870(ctx, base); return;
  }
  WorldScope scope(base, ctx.r3.u32, ctx.r5.u32);
  __imp__sub_82515870(ctx, base);
}

extern "C" void sub_8259AAA8(PPCContext& ctx, uint8_t* base) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  const bool observed = manager == kManager && index < kInvalidEntry &&
                        (config.modern || trace_enabled.load(std::memory_order_relaxed));
  if (observed) {
    const uint32_t table = REX_LOAD_U32(manager);
    if (table && table == REX_LOAD_U32(kEntriesGlobal)) {
      BindTable(table);
      const uint32_t entry = table + index * kEntryStride;
      const uint32_t state = REX_LOAD_U32(entry + 8) >> kStateShift;
      if (state == 0 || state == 2) {
        // Deadlines use the view snapshot; latency starts at actual admission,
        // not at the beginning of a possibly long CPU frame.
        const uint64_t now = Now();
        const bool urgent = (ctx.r5.u32 & 0x10) != 0 ||
                            (world.deadline && world.deadline <= now);
        const uint64_t deadline = urgent ? now :
            world.deadline ? world.deadline : SaturatingAdd(now, 250000000);
        deadlines.Request(index, deadline);
        if (config.modern && world.entity && urgent) ctx.r5.u32 |= 0x10;
        std::lock_guard lock(records_mutex);
        Record& record = records[index];
        if (record.table != table) { record = {}; record.table = table; }
        if (!record.request_time) record.request_time = now;
        if (world.entity) { record.entity = world.entity; record.model = world.model; ++world_requests; }
        EventLocked({now, 0, record.generation + (state == 0 ? 1 : 0), index, record.entity, record.model,
                     1, state, state, REX_LOAD_U32(entry + 8) & kSizeMask, deadline});
      }
    }
  }
  __imp__sub_8259AAA8(ctx, base);
  if (observed) {
    pending_count.store(REX_LOAD_U32(manager + 56), std::memory_order_relaxed);
    const uint32_t table = REX_LOAD_U32(manager);
    if (ctx.r3.u32 == 0 && table && table == REX_LOAD_U32(kEntriesGlobal) &&
        (REX_LOAD_U32(table + index * kEntryStride + 8) >> kStateShift) == 0) {
      std::lock_guard lock(records_mutex);
      deadlines.Release(index);
      records[index].request_time = 0;
      records[index].entity = 0;
      records[index].model = 0;
    }
  }
}

extern "C" void sub_8259A250(PPCContext& ctx, uint8_t* base) {
  if (!config.modern && !trace_enabled.load(std::memory_order_relaxed)) {
    __imp__sub_8259A250(ctx, base);
    return;
  }
  const uint32_t entry = ctx.r3.u32;
  const uint32_t before = REX_LOAD_U32(entry + 8) >> kStateShift;
  __imp__sub_8259A250(ctx, base);
  if (!config.modern && !trace_enabled.load(std::memory_order_relaxed)) return;
  const uint32_t table = REX_LOAD_U32(kEntriesGlobal);
  const uint32_t index = EntryIndex(table, entry);
  if (index == kInvalidEntry) return;
  BindTable(table);
  const uint32_t packed = REX_LOAD_U32(entry + 8);
  const uint32_t after = packed >> kStateShift;
  if (before == after) return;
  const uint64_t now = Now();
  std::lock_guard lock(records_mutex);
  Record& record = records[index];
  if (record.table != table) { record = {}; record.table = table; }
  if (before == 0 && after != 0) {
    ++record.generation;
    if (record.release_time && now - record.release_time < 2000000000ULL) ++reloads;
    if (!record.request_time) record.request_time = now;
  }
  if (after == 1 && record.request_time && record.entity && !record.ready_time) {
    record.ready_time = now;
    const uint64_t latency = std::min(now - record.request_time, uint64_t{2000000000});
    const uint64_t prior = observed_world_latency.load(std::memory_order_relaxed);
    observed_world_latency.store((prior * 7 + latency) / 8, std::memory_order_relaxed);
  }
  ++state_changes;
  EventLocked({now, 0, record.generation, index, record.entity, record.model,
               2, before, after, packed & kSizeMask, record.request_time});
  if (after == 0) {
    deadlines.Release(index);
    record.request_time = 0; record.ready_time = 0;
    record.entity = 0; record.model = 0; record.release_time = now;
  }
}

extern "C" void sub_8259A610(PPCContext& ctx, uint8_t* base) {
  const uint32_t manager = ctx.r3.u32, requested = ctx.r4.u32;
  if (config.modern && manager == kManager) {
    ctx.r4.u32 = RequestedBudget(requested, config.budget_scale);
  }
  const uint32_t target = ctx.r4.u32;
  __imp__sub_8259A610(ctx, base);
  if (manager == kManager) REXLOG_INFO("gta4-streaming: budget virtual original={} target={} effective={}",
                                     requested, target, REX_LOAD_U32(manager + 32));
}
extern "C" void sub_8259A698(PPCContext& ctx, uint8_t* base) {
  const uint32_t manager = ctx.r3.u32, requested = ctx.r4.u32;
  if (config.modern && manager == kManager) {
    ctx.r4.u32 = RequestedBudget(requested, config.budget_scale);
  }
  const uint32_t target = ctx.r4.u32;
  __imp__sub_8259A698(ctx, base);
  if (manager == kManager) {
    const uint32_t allocator_clamped = REX_LOAD_U32(manager + 44);
    const uint32_t effective = config.modern
        ? BudgetWithHeadroom(requested, allocator_clamped, config.physical_reserve_bytes)
        : allocator_clamped;
    REX_STORE_U32(manager + 44, effective);
    REXLOG_INFO("gta4-streaming: budget physical original={} target={} allocator-clamped={} "
                "reserve={} effective={}", requested, target, allocator_clamped,
                config.physical_reserve_bytes.load(), effective);
  }
}

// Mechanically derived complete functions. Only the preload operand and queue
// ranking sites differ; the generator verifies source hashes and site counts.
#include "gta4_streaming_guest.inc"
