#include "input/context_touch_activity.h"
#include "input/context_touch_context.h"

#include <array>
#include <limits>
#include <optional>

namespace gta4::input {
namespace {
// Derived from compiled sub_82843ED0, sub_82845598 and sub_82845338. The
// table count is uint16; a live thread has serial+4, program+8, status+12,
// and its local storage at +80. No IDA pseudocode participates in this reader.
constexpr uint32_t kThreads = 0x83192608;
constexpr uint32_t kCurrentThread = 0x831925FC;
constexpr uint32_t kEpisode = 0x82B39384;
constexpr size_t kMaximumThreads = 1024;
struct Reader {
  const TouchContextMemory& memory;
  template<class T> std::optional<T> Read(uint32_t address, uint32_t offset = 0) const noexcept {
    const uint64_t end = uint64_t(address) + offset + sizeof(T);
    if (!address || !memory.read || end > uint64_t(UINT32_MAX) + 1) return std::nullopt;
    T value{};
    if (!memory.read(memory.opaque, address + offset, &value, sizeof(value))) return std::nullopt;
    if constexpr (sizeof(T) == 4) value = __builtin_bswap32(value);
    if constexpr (sizeof(T) == 2) value = __builtin_bswap16(value);
    return value;
  }
};

TouchActivitySnapshot ReadThread(const Reader& read, uint32_t thread, size_t index,
                                bool minigame, bool gameplay) noexcept {
  if (index >= kTouchActivityProfiles.size()) return {};
  const auto& p = kTouchActivityProfiles[index];
  const auto id = read.Read<uint32_t>(thread, 4);
  const auto key = read.Read<uint32_t>(thread, 8);
  const auto status = read.Read<uint32_t>(thread, 12);
  const auto locals = read.Read<uint32_t>(thread, 80);
  if (!id || !*id || !key || *key != p.program_key || !status || *status >= 2 || !locals || !*locals)
    return {};
  std::array<uint32_t, 8> state{};
  for (size_t i = 0; i < p.locals.size(); ++i) {
    if (p.locals[i] == UINT16_MAX) continue;
    if (p.locals[i] >= p.local_count) return {};
    const auto value = read.Read<uint32_t>(*locals, uint32_t(p.locals[i]) * sizeof(uint32_t));
    if (!value) return {};
    state[i] = *value;
  }
  auto result = ClassifyTouchActivity(index, state, minigame, gameplay);
  if (result.valid) result.script_thread = *id;
  return result;
}

bool ValidProgram(const Reader& read, uint32_t program, const TouchActivityProfile& p) noexcept {
  if (!program) return false;
  const auto key = read.Read<uint32_t>(program, 4);
  const auto size = read.Read<uint32_t>(program, 16);
  // sub_828463B8 copies the SCO local count to +20. The adjacent +22
  // halfword comes from the SCO flags and is not a local count.
  const auto locals = read.Read<uint16_t>(program, 20);
  return key && *key == p.program_key && size && *size == p.code_size && locals && *locals == p.local_count;
}
}  // namespace

TouchActivitySnapshot ReadTouchActivityFacts(const TouchContextMemory& memory,
                                             const TouchContextSnapshot& context) noexcept {
  if (!context.valid || !context.playing || !context.native_input_allowed || context.loading ||
      context.cutscene || context.frontend || context.map ||
      (!context.minigame_active && context.gameplay_allowed)) return {};
  const Reader read{memory};
  const auto episode = read.Read<uint32_t>(kEpisode);
  if (!episode || *episode > 2) return {};
  const auto registered = TouchActivityPrograms();
  std::array<bool, kTouchActivityProfiles.size()> admitted{};
  bool any = false;
  for (size_t i = 0; i < admitted.size(); ++i) {
    const auto& p = kTouchActivityProfiles[i];
    admitted[i] = p.episode == *episode && ValidProgram(read, registered[i], p);
    any |= admitted[i];
  }
  if (!any) return {};
  const auto count = read.Read<uint16_t>(kThreads, 4);
  const auto table = read.Read<uint32_t>(kThreads);
  if (!count || !*count || *count > kMaximumThreads || !table || !*table) return {};
  TouchActivitySnapshot result;
  for (uint32_t i = 0; i < *count; ++i) {
    const auto thread = read.Read<uint32_t>(*table, i * sizeof(uint32_t));
    if (!thread || !*thread) continue;
    const auto key = read.Read<uint32_t>(*thread, 8);
    if (!key) continue;
    for (size_t profile = 0; profile < admitted.size(); ++profile) {
      if (!admitted[profile] || kTouchActivityProfiles[profile].program_key != *key) continue;
      auto candidate = ReadThread(read, *thread, profile, context.minigame_active, context.gameplay_allowed);
      if (!candidate.valid) continue;
      // Conflicting owners are not resolved by table order. Keep the original
      // generic query UI until a unique, verified activity is identifiable.
      if (result.valid && !result.SameSession(candidate)) return {};
      result = candidate;
    }
  }
  return result;
}

bool TouchActivityQueryMatches(const TouchContextMemory& memory,
                               const TouchActivitySnapshot& expected) noexcept {
  if (!expected.valid) return true;
  if (!expected.profile || expected.profile > kTouchActivityProfiles.size()) return false;
  const Reader read{memory};
  const auto current = read.Read<uint32_t>(kCurrentThread);
  const auto episode = read.Read<uint32_t>(kEpisode);
  if (!current || !*current || !episode || *episode != expected.episode) return false;
  const auto index = expected.profile - 1;
  const auto programs = TouchActivityPrograms();
  if (!ValidProgram(read, programs[index], kTouchActivityProfiles[index])) return false;
  auto now = ReadThread(read, *current, index, true, expected.native_combat);
  return now.valid && CompatibleActivityContacts(expected, now);
}
}  // namespace gta4::input
