#include "gpu_pass_timer.h"

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/logging.h>

namespace rex::graphics::gta4_metal::gpu_pass_timer {
namespace {

constexpr NSUInteger kSamples = 4096;   // Metal caps a counter buffer at 32 KiB (4096 timestamps)
constexpr size_t kBuffers = 8;          // rotated as they fill: ~16k passes in flight
constexpr uint32_t kFramesPerRow = 300;

struct Totals {
  double ns = 0;
  uint64_t passes = 0;
};

struct State {
  id<MTLDevice> device = nil;
  std::vector<id<MTLCounterSampleBuffer>> buffers;
  size_t buffer = 0;
  std::string path;
  NSUInteger next = 0;
  struct Pass { id<MTLCounterSampleBuffer> buffer; NSUInteger first; std::string category; };
  std::vector<Pass> pending;
  std::mutex mutex;                                        // guards totals (completion threads)
  std::map<std::string, Totals> totals;
  double ns_per_tick = 1.0;
  uint32_t frames = 0;
  uint64_t rows = 0;
};
std::unique_ptr<State> g;

double CalibrateNsPerTick(id<MTLDevice> device) {
  MTLTimestamp cpu0 = 0, gpu0 = 0, cpu1 = 0, gpu1 = 0;
  [device sampleTimestamps:&cpu0 gpuTimestamp:&gpu0];
  [NSThread sleepForTimeInterval:0.02];
  [device sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
  return gpu1 > gpu0 ? double(cpu1 - cpu0) / double(gpu1 - gpu0) : 1.0;
}

}  // namespace

void Initialize(id<MTLDevice> device, const std::string& csv_path) {
  if (csv_path.empty() || g) return;
  if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
    REXLOG_WARN("gta4-metal-gpu-pass: stage-boundary counter sampling unsupported; disabled");
    return;
  }
  id<MTLCounterSet> timestamps = nil;
  for (id<MTLCounterSet> set in device.counterSets)
    if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) timestamps = set;
  if (!timestamps) { REXLOG_WARN("gta4-metal-gpu-pass: no timestamp counter set; disabled"); return; }
  auto descriptor = [MTLCounterSampleBufferDescriptor new];
  descriptor.counterSet = timestamps;
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.sampleCount = kSamples;
  NSError* error = nil;
  std::vector<id<MTLCounterSampleBuffer>> buffers;
  for (size_t i = 0; i < kBuffers; ++i) {
    auto buffer = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
    if (!buffer) {
      REXLOG_WARN("gta4-metal-gpu-pass: counter buffer: {}", error ? error.localizedDescription.UTF8String : "failed");
      return;
    }
    buffers.push_back(buffer);
  }
  g = std::make_unique<State>();
  g->device = device;
  g->buffers = std::move(buffers);
  g->path = csv_path;
  g->ns_per_tick = CalibrateNsPerTick(device);
  std::ofstream(csv_path, std::ios::trunc) << "row,frames,category,ms_per_frame,passes_per_frame\n";
  REXLOG_INFO("gta4-metal-gpu-pass: enabled ({:.3f} ns/tick) -> {}", g->ns_per_tick, csv_path);
}

bool enabled() { return g != nullptr; }

void Tag(MTLRenderPassDescriptor* pass, std::string category) {
  if (!g || !pass) return;
  if (g->next + 2 > kSamples) { g->next = 0; g->buffer = (g->buffer + 1) % kBuffers; }
  auto attachment = pass.sampleBufferAttachments[0];
  attachment.sampleBuffer = g->buffers[g->buffer];
  attachment.startOfVertexSampleIndex = g->next;
  attachment.endOfVertexSampleIndex = MTLCounterDontSample;
  attachment.startOfFragmentSampleIndex = MTLCounterDontSample;
  attachment.endOfFragmentSampleIndex = g->next + 1;
  g->pending.push_back({g->buffers[g->buffer], g->next, std::move(category)});
  g->next += 2;
}

void Commit(id<MTLCommandBuffer> commands) {
  if (!g || g->pending.empty() || !commands) return;
  auto passes = std::make_shared<std::vector<State::Pass>>(std::move(g->pending));
  g->pending.clear();
  State* state = g.get();
  [commands addCompletedHandler:^(id<MTLCommandBuffer>) {
    std::map<std::string, Totals> local;
    for (const auto& [buffer, first, category] : *passes) {
      NSData* data = [buffer resolveCounterRange:NSMakeRange(first, 2)];
      if (!data || data.length < 2 * sizeof(MTLCounterResultTimestamp)) continue;
      const auto* stamps = static_cast<const MTLCounterResultTimestamp*>(data.bytes);
      if (stamps[0].timestamp == MTLCounterErrorValue || stamps[1].timestamp == MTLCounterErrorValue ||
          stamps[1].timestamp <= stamps[0].timestamp)
        continue;
      auto& t = local[category];
      t.ns += double(stamps[1].timestamp - stamps[0].timestamp) * state->ns_per_tick;
      ++t.passes;
    }
    std::lock_guard lock(state->mutex);
    for (const auto& [category, t] : local) {
      auto& total = state->totals[category];
      total.ns += t.ns;
      total.passes += t.passes;
    }
  }];
}

void Count(const std::string& key) {
  if (!g) return;
  std::lock_guard lock(g->mutex);
  ++g->totals["#" + key].passes;
}

void FrameEnd() {
  if (!g || ++g->frames < kFramesPerRow) return;
  std::map<std::string, Totals> totals;
  {
    std::lock_guard lock(g->mutex);
    totals.swap(g->totals);
  }
  std::ofstream out(g->path, std::ios::app);
  ++g->rows;
  for (const auto& [category, t] : totals)
    out << g->rows << ',' << g->frames << ",\"" << category << "\"," << t.ns / 1e6 / g->frames << ','
        << double(t.passes) / g->frames << '\n';
  g->frames = 0;
}

}  // namespace rex::graphics::gta4_metal::gpu_pass_timer
