#include "shaders.h"

#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <list>
#include <span>
#include <unordered_map>
#include <utility>

#include "shader_archive.h"
#include "../../ui/metal/context.h"

namespace rex::graphics::gta4_metal {
namespace {
struct FunctionKey {
  uint64_t hash = 0;
  uint32_t specialization = 0;
  bool late = false,isolated_ui=false;
  bool operator==(const FunctionKey&) const = default;
};
struct FunctionKeyHash {
  size_t operator()(const FunctionKey& key) const noexcept {
    uint64_t value = key.hash ^ (uint64_t(key.specialization) << 1) ^ uint64_t(key.late) ^ (uint64_t(key.isolated_ui)<<63);
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return size_t(value);
  }
};
struct LibraryKey {
  uint64_t hash = 0;
  bool late = false;
  bool operator==(const LibraryKey&) const = default;
};
struct LibraryKeyHash {
  size_t operator()(const LibraryKey& key) const noexcept {
    return FunctionKeyHash{}({key.hash, 0, key.late});
  }
};
}  // namespace

// Single renderer/compilation owner: this cache is not an additional scheduler.
// Returned Metal objects have normal ARC ownership and may outlive LRU eviction.
struct ShaderCache::Impl {
  explicit Impl(std::shared_ptr<ui::metal::MetalContext> value) : context(std::move(value)) {}
  struct FunctionEntry {
    id<MTLFunction> function = nil;
    std::list<FunctionKey>::iterator position;
  };
  struct LibraryEntry {
    id<MTLLibrary> library = nil;
    std::list<LibraryKey>::iterator position;
    size_t bytes = 0;
  };
  std::shared_ptr<ui::metal::MetalContext> context;
  std::string resource_name;
  NSData* file = nil;
  MetalShaderArchive index;
  std::unordered_map<uint64_t,ShaderMetadata> metadata;
  std::list<FunctionKey> function_lru;
  std::list<LibraryKey> library_lru;
  std::unordered_map<FunctionKey, FunctionEntry, FunctionKeyHash> functions;
  std::unordered_map<LibraryKey, LibraryEntry, LibraryKeyHash> libraries;
  size_t library_bytes = 0;
  static constexpr size_t kMaximumFunctions = 512;
  static constexpr size_t kMaximumLibraries = 96;
  static constexpr size_t kMaximumLibraryPayloadBytes = 32u * 1024u * 1024u;

  id<MTLLibrary> Library(const MetalShaderRecord& record, bool late, std::string& error) {
    const LibraryKey key{record.hash, late};
    if (auto existing = libraries.find(key); existing != libraries.end()) {
      library_lru.splice(library_lru.begin(), library_lru, existing->second.position);
      return existing->second.library;
    }
    const auto bytes = late ? record.late_library : record.early_library;
    if (bytes.empty()) { error = "Requested Metal shader variant is absent"; return nil; }
    // dispatch_data owns a strong reference to the mapping through its block.
    // The compiler may consume the bytes after this method returns.
    NSData* mapping = file;
    auto data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, ^{ (void)mapping; });
    NSError* native_error = nil;
    auto library = [context->device newLibraryWithData:data error:&native_error];
    if (!library) { error = ui::metal::MetalError(native_error, "Unable to load Metal shader library"); return nil; }
    library.label = [[NSString alloc] initWithBytes:record.name.data() length:record.name.size() encoding:NSUTF8StringEncoding];
    // A single unusually large library can be used without permanently enlarging
    // the cache. Metal pipeline/function objects retain their required state.
    if (bytes.size() <= kMaximumLibraryPayloadBytes) {
      for (; !library_lru.empty() && (libraries.size() >= kMaximumLibraries ||
              bytes.size() > kMaximumLibraryPayloadBytes - library_bytes);) {
        auto oldest = libraries.find(library_lru.back());
        library_bytes -= oldest->second.bytes;
        libraries.erase(oldest);
        library_lru.pop_back();
      }
      library_lru.push_front(key);
      libraries.emplace(key, LibraryEntry{library, library_lru.begin(), bytes.size()});
      library_bytes += bytes.size();
    }
    return library;
  }
};

ShaderCache::ShaderCache(std::shared_ptr<ui::metal::MetalContext> context, std::string resource_name)
    : impl_(std::make_unique<Impl>(std::move(context))) { impl_->resource_name = std::move(resource_name); }
ShaderCache::~ShaderCache() = default;

bool ShaderCache::Initialize(std::string& error) {
  @autoreleasepool {
    if (!impl_->index.records().empty()) { error.clear(); return true; }
    NSString* name = [NSString stringWithUTF8String:impl_->resource_name.c_str()];
    NSURL* resource = [[NSBundle mainBundle] URLForResource:name withExtension:@"bin" subdirectory:@"metal"];
    if (!resource) { error = "Bundle is missing metal/" + impl_->resource_name + ".bin"; return false; }
    return InitializeFile(resource.path.UTF8String, error);
  }
}

bool ShaderCache::InitializeFile(const std::string& path, std::string& error) {
  @autoreleasepool {
    error.clear();
    if (!impl_->context || !impl_->context->device) { error = "Metal shader cache requires a device"; return false; }
    if (!impl_->index.records().empty()) { error = "Shader archive already initialized"; return false; }
    NSURL* resource = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    NSNumber* file_size = nil;
    NSError* native_error = nil;
    if (![resource getResourceValue:&file_size forKey:NSURLFileSizeKey error:&native_error] ||
        !file_size || file_size.unsignedLongLongValue > kMetalArchiveMaximumBytes) {
      error = "Metal shader archive exceeds its file-size bound"; return false;
    }
    NSData* bytes = [NSData dataWithContentsOfURL:resource options:NSDataReadingMappedIfSafe error:&native_error];
    if (!bytes) { error = ui::metal::MetalError(native_error, "Cannot map Metal shader archive"); return false; }
    const auto digest = [](std::span<const std::byte> data, std::span<const std::byte, 32> expected) {
      std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> actual{};
      CC_SHA256(data.data(), CC_LONG(data.size()), actual.data());
      return std::memcmp(actual.data(), expected.data(), actual.size()) == 0;
    };
    if (!impl_->index.Open({static_cast<const std::byte*>(bytes.bytes), bytes.length}, digest, error)) return false;
    impl_->file = bytes;
    return true;
  }
}

std::optional<ShaderMetadata> ShaderCache::Lookup(uint64_t hash,gta4_native::ShaderStage stage,std::string& error) {
  const auto* value=Metadata(hash,stage,error);
  return value?std::optional(*value):std::nullopt;
}

const ShaderMetadata* ShaderCache::Metadata(uint64_t hash,gta4_native::ShaderStage stage,std::string& error) {
  error.clear();
  if(auto existing=impl_->metadata.find(hash);existing!=impl_->metadata.end()){
    if(existing->second.stage!=stage){error="Metal shader stage disagrees with cached metadata";return nullptr;}
    return &existing->second;
  }
  const auto* record = impl_->index.Find(hash, MetalArchiveStage(uint32_t(stage)));
  if (!record) {
    char detail[96];
    std::snprintf(detail, sizeof(detail), " (hash=%016llX stage=%u)", static_cast<unsigned long long>(hash),
                  static_cast<unsigned>(stage));
    error = std::string("Metal shader hash or stage is not present in the archive") + detail;
    return nullptr;
  }
  ShaderMetadata result;
  result.hash = record->hash;
  result.stage = stage;
  result.used_texture_mask = record->used_texture_mask;
  result.specialization_constants_mask = record->specialization_constants_mask;
  result.color_output_mask = record->output_mask & 15;
  result.writes_depth = (record->output_mask & 16) != 0;
  result.late_available = !record->late_library.empty();
  result.name = record->name;
  result.attribute_count = record->attribute_count;
  result.attributes = record->attributes;
  result.native_output = (record->flags & kMetalShaderNativeOutput) != 0;
  result.coverage_output = (record->flags & kMetalShaderCoverage) != 0;
  return &impl_->metadata.emplace(hash,std::move(result)).first->second;
}

id<MTLFunction> ShaderCache::Function(uint64_t hash, gta4_native::ShaderStage stage,
                                     uint32_t specialization, bool late, std::string& error, bool isolated_ui) {
  @autoreleasepool {
    error.clear();
    const auto* record = impl_->index.Find(hash, MetalArchiveStage(uint32_t(stage)));
    if (!record) { error = "Metal shader hash or stage is absent"; return nil; }
    if (late && record->late_library.empty()) { error = "Required late-test Metal shader is absent"; return nil; }
    const FunctionKey key{hash, specialization & record->specialization_constants_mask, late,isolated_ui};
    if (auto existing = impl_->functions.find(key); existing != impl_->functions.end()) {
      impl_->function_lru.splice(impl_->function_lru.begin(), impl_->function_lru, existing->second.position);
      return existing->second.function;
    }
    auto library = impl_->Library(*record, late, error);
    if (!library) return nil;
    auto constants = [[MTLFunctionConstantValues alloc] init];
    if (record->specialization_constants_mask)
      [constants setConstantValue:&key.specialization type:MTLDataTypeUInt atIndex:0];
    NSError* native_error = nil;
    auto function = [library newFunctionWithName:isolated_ui?@"shaderMainUI":@"shaderMain" constantValues:constants error:&native_error];
    if (!function) { error = ui::metal::MetalError(native_error, "Metal function specialization failed"); return nil; }
    const auto expected_type = stage == gta4_native::ShaderStage::kVertex ? MTLFunctionTypeVertex : MTLFunctionTypeFragment;
    if (function.functionType != expected_type) { error = "Metal library function stage disagrees with its archive"; return nil; }
    if (impl_->functions.size() >= Impl::kMaximumFunctions) {
      impl_->functions.erase(impl_->function_lru.back());
      impl_->function_lru.pop_back();
    }
    impl_->function_lru.push_front(key);
    impl_->functions.emplace(key, Impl::FunctionEntry{function, impl_->function_lru.begin()});
    return function;
  }
}

void ShaderCache::Clear() {
  impl_->functions.clear();
  impl_->function_lru.clear();
  impl_->libraries.clear();
  impl_->library_lru.clear();
  impl_->library_bytes = 0;
  impl_->metadata.clear();
  impl_->index.Clear();
  impl_->file = nil;
}
}  // namespace rex::graphics::gta4_metal
