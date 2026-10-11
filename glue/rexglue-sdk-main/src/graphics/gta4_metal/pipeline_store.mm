#include "pipeline_store.h"

#include <climits>
#include <cstring>
#include <unistd.h>
#include <rex/logging.h>
#include <xxhash.h>
#include "../../ui/metal/context.h"

namespace rex::graphics::gta4_metal {

namespace {
constexpr const char* kRebuildMarker = "rebuild-pending";
}

std::filesystem::path PipelineStore::Directory(const std::filesystem::path& cache_root, uint32_t title_id) {
  char title[16];
  std::snprintf(title, sizeof(title), "%08X", title_id);
  return cache_root / "gta4-metal" / title;
}

void PipelineStore::PrepareLaunch(const std::filesystem::path& cache_root, uint32_t title_id) {
  std::error_code ec;
  if (!std::filesystem::exists(Directory(cache_root, title_id) / kRebuildMarker, ec)) return;
  // Apple keeps a per-app shader cache under the user cache directory. Pipelines served from it are
  // not compiled in this process, and serializing a binary archive that holds one of them can fault
  // inside Metal (see pipeline_store.h). The marker says this launch rebuilds, so start that cache
  // empty; everything it held is recompiled into our archive by the rebuild anyway.
  char user_cache[PATH_MAX];
  const size_t length = confstr(_CS_DARWIN_USER_CACHE_DIR, user_cache, sizeof(user_cache));
  NSString* bundle = [[NSBundle mainBundle] bundleIdentifier];
  if (!length || length > sizeof(user_cache) || !bundle) {
    REXLOG_WARN("gta4-metal-pipeline-cache: pipeline rebuild pending but the host shader cache "
                "location is unknown; leaving it in place");
    return;
  }
  const auto host_cache = std::filesystem::path(user_cache) / bundle.UTF8String / "com.apple.metal";
  if (!std::filesystem::exists(host_cache, ec)) return;
  uintmax_t bytes = 0, files = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(host_cache, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    ++files;
    bytes += entry.file_size(ec);
  }
  std::filesystem::remove_all(host_cache, ec);
  if (ec) {
    REXLOG_WARN("gta4-metal-pipeline-cache: could not clear the host shader cache {}: {}",
                host_cache.string(), ec.message());
    return;
  }
  REXLOG_WARN("gta4-metal-pipeline-cache: pipeline rebuild pending; cleared the host shader cache {} "
              "({} files, {:.1f} MB) so the rebuilt archive can be serialized",
              host_cache.string(), files, double(bytes) / (1024.0 * 1024.0));
}

PipelineRecipe RecipeFromDescriptor(MTLRenderPipelineDescriptor* descriptor, const RecipeFunction& vertex,
                                    const RecipeFunction& fragment) {
  PipelineRecipe recipe;
  std::memset(&recipe, 0, sizeof(recipe));  // padding participates in the hash
  recipe.version = PipelineRecipe::kVersion;
  recipe.samples = uint32_t(descriptor.rasterSampleCount);
  recipe.depth_format = uint32_t(descriptor.depthAttachmentPixelFormat);
  recipe.stencil_format = uint32_t(descriptor.stencilAttachmentPixelFormat);
  recipe.vertex = vertex;
  recipe.fragment = fragment;
  for (uint32_t i = 0; i < PipelineRecipe::kColors; ++i) {
    auto attachment = descriptor.colorAttachments[i];
    auto& color = recipe.colors[i];
    color.format = uint32_t(attachment.pixelFormat);
    if (!color.format) continue;
    color.write_mask = uint32_t(attachment.writeMask);
    color.blending = attachment.blendingEnabled ? 1 : 0;
    color.src_rgb = uint8_t(attachment.sourceRGBBlendFactor);
    color.dst_rgb = uint8_t(attachment.destinationRGBBlendFactor);
    color.src_a = uint8_t(attachment.sourceAlphaBlendFactor);
    color.dst_a = uint8_t(attachment.destinationAlphaBlendFactor);
    color.op_rgb = uint8_t(attachment.rgbBlendOperation);
    color.op_a = uint8_t(attachment.alphaBlendOperation);
  }
  auto layout = descriptor.vertexDescriptor;
  for (uint32_t i = 0; layout && i < PipelineRecipe::kAttributes; ++i) {
    auto attribute = layout.attributes[i];
    if (attribute.format == MTLVertexFormatInvalid) continue;
    recipe.attributes[i] = {uint32_t(attribute.format), uint32_t(attribute.offset), uint32_t(attribute.bufferIndex)};
  }
  for (uint32_t i = 0; layout && i < PipelineRecipe::kLayouts; ++i) {
    auto buffer = layout.layouts[i];
    if (!buffer.stride) continue;
    recipe.layouts[i] = {uint32_t(buffer.stride), uint32_t(buffer.stepFunction)};
  }
  return recipe;
}

MTLRenderPipelineDescriptor* DescriptorFromRecipe(const PipelineRecipe& recipe, id<MTLFunction> vertex,
                                                  id<MTLFunction> fragment) {
  auto descriptor = [MTLRenderPipelineDescriptor new];
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.rasterSampleCount = recipe.samples;
  descriptor.alphaToCoverageEnabled = NO;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormat(recipe.depth_format);
  descriptor.stencilAttachmentPixelFormat = MTLPixelFormat(recipe.stencil_format);
  for (uint32_t i = 0; i < PipelineRecipe::kColors; ++i) {
    const auto& color = recipe.colors[i];
    if (!color.format) continue;
    auto attachment = descriptor.colorAttachments[i];
    attachment.pixelFormat = MTLPixelFormat(color.format);
    attachment.writeMask = MTLColorWriteMask(color.write_mask);
    attachment.blendingEnabled = color.blending ? YES : NO;
    attachment.sourceRGBBlendFactor = MTLBlendFactor(color.src_rgb);
    attachment.destinationRGBBlendFactor = MTLBlendFactor(color.dst_rgb);
    attachment.sourceAlphaBlendFactor = MTLBlendFactor(color.src_a);
    attachment.destinationAlphaBlendFactor = MTLBlendFactor(color.dst_a);
    attachment.rgbBlendOperation = MTLBlendOperation(color.op_rgb);
    attachment.alphaBlendOperation = MTLBlendOperation(color.op_a);
  }
  auto layout = [MTLVertexDescriptor vertexDescriptor];
  for (uint32_t i = 0; i < PipelineRecipe::kAttributes; ++i) {
    const auto& attribute = recipe.attributes[i];
    if (!attribute.format) continue;
    layout.attributes[i].format = MTLVertexFormat(attribute.format);
    layout.attributes[i].offset = attribute.offset;
    layout.attributes[i].bufferIndex = attribute.buffer;
  }
  for (uint32_t i = 0; i < PipelineRecipe::kLayouts; ++i) {
    if (!recipe.layouts[i].stride) continue;
    layout.layouts[i].stride = recipe.layouts[i].stride;
    layout.layouts[i].stepFunction = MTLVertexStepFunction(recipe.layouts[i].step);
  }
  descriptor.vertexDescriptor = layout;
  return descriptor;
}

uint64_t HashRecipe(const PipelineRecipe& recipe) { return XXH3_64bits(&recipe, sizeof(recipe)); }

bool PipelineStore::Open(id<MTLDevice> device, const std::filesystem::path& cache_root, uint32_t title_id,
                         uint64_t shader_identity, std::string& error) {
  std::lock_guard lock(mutex_);
  device_ = device;
  directory_ = Directory(cache_root, title_id);
  std::error_code ec;
  std::filesystem::create_directories(directory_, ec);
  if (ec) { error = "Cannot create Metal pipeline cache directory: " + ec.message(); return false; }

  const std::string os = [[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String];
  const std::string gpu = [device.name UTF8String];
  const uint64_t identity_parts[3] = {XXH3_64bits(os.data(), os.size()), XXH3_64bits(gpu.data(), gpu.size()),
                                      shader_identity};
  char identity[32];
  std::snprintf(identity, sizeof(identity), "%016llX",
                static_cast<unsigned long long>(XXH3_64bits(identity_parts, sizeof(identity_parts))));
  recipes_path_ = directory_ / "recipes-v1.bin";
  archive_path_ = directory_ / (std::string("pipelines-") + identity + ".metalar");
  manifest_path_ = directory_ / (std::string("pipelines-") + identity + ".manifest");
  marker_path_ = directory_ / kRebuildMarker;
  marker_present_ = std::filesystem::exists(marker_path_, ec);

  // Recipe log: a sequence of fixed-size records; a torn tail record is ignored.
  if (std::ifstream in(recipes_path_, std::ios::binary); in) {
    PipelineRecipe recipe;
    while (in.read(reinterpret_cast<char*>(&recipe), sizeof(recipe))) {
      if (recipe.version != PipelineRecipe::kVersion) continue;
      if (recorded_.insert(HashRecipe(recipe)).second) recipes_.push_back(recipe);
    }
  }
  if (std::ifstream in(manifest_path_, std::ios::binary); in) {
    uint64_t hash;
    while (in.read(reinterpret_cast<char*>(&hash), sizeof(hash))) archived_.insert(hash);
  }

  auto descriptor = [MTLBinaryArchiveDescriptor new];
  if (std::filesystem::exists(archive_path_) && !archived_.empty())
    descriptor.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:archive_path_.c_str()]];
  NSError* native_error = nil;
  archive_ = [device newBinaryArchiveWithDescriptor:descriptor error:&native_error];
  if (!archive_ && descriptor.url) {
    // A stale or corrupt archive is rebuilt from the recipe log.
    REXLOG_WARN("gta4-metal-pipeline-cache: discarding unreadable archive: {}",
                ui::metal::MetalError(native_error, "load failed"));
    archived_.clear();
    descriptor.url = nil;
    archive_ = [device newBinaryArchiveWithDescriptor:descriptor error:&native_error];
  }
  if (!archive_) { error = ui::metal::MetalError(native_error, "Metal binary archive creation failed"); return false; }
  if (!descriptor.url) archived_.clear();

  log_.open(recipes_path_, std::ios::binary | std::ios::app);
  REXLOG_INFO("gta4-metal-pipeline-cache: {} recipes recorded, {} archived ({})", recipes_.size(), archived_.size(),
              archive_path_.filename().string());
  return true;
}

void PipelineStore::Record(const PipelineRecipe& recipe) {
  std::lock_guard lock(mutex_);
  if (!archive_ || !recorded_.insert(HashRecipe(recipe)).second) return;
  recipes_.push_back(recipe);
  if (log_) {
    log_.write(reinterpret_cast<const char*>(&recipe), sizeof(recipe));
    log_.flush();
  }
  // This pipeline was buildable (it was just built), so the next launch rebuilds the archive.
  TouchRebuildMarker();
}

void PipelineStore::TouchRebuildMarker() {
  if (marker_present_ || marker_path_.empty()) return;
  std::ofstream(marker_path_, std::ios::binary | std::ios::trunc) << "pipeline rebuild pending\n";
  marker_present_ = true;
}

void PipelineStore::ClearRebuildMarker() {
  if (marker_path_.empty()) return;
  std::error_code ec;
  std::filesystem::remove(marker_path_, ec);
  marker_present_ = false;
}

void PipelineStore::Settle() {
  std::lock_guard lock(mutex_);
  ClearRebuildMarker();
}

std::vector<PipelineRecipe> PipelineStore::Pending() const {
  std::lock_guard lock(mutex_);
  std::vector<PipelineRecipe> pending;
  for (const auto& recipe : recipes_)
    if (!archived_.contains(HashRecipe(recipe))) pending.push_back(recipe);
  return pending;
}

std::vector<PipelineRecipe> PipelineStore::All() const {
  std::lock_guard lock(mutex_);
  return recipes_;
}

bool PipelineStore::Reset(std::string& error) {
  std::lock_guard lock(mutex_);
  NSError* native_error = nil;
  auto fresh = [device_ newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&native_error];
  if (!fresh) { error = ui::metal::MetalError(native_error, "Metal binary archive creation failed"); return false; }
  archive_ = fresh;
  archived_.clear();
  dirty_ = true;
  // Stays until Save() succeeds: a crash or force-quit during the rebuild rebuilds again next
  // launch, with the host shader cache cleared first (PrepareLaunch).
  TouchRebuildMarker();
  return true;
}

bool PipelineStore::Add(MTLRenderPipelineDescriptor* descriptor, uint64_t recipe_hash, std::string& error) {
  NSError* native_error = nil;
  if (![archive_ addRenderPipelineFunctionsWithDescriptor:descriptor error:&native_error]) {
    error = ui::metal::MetalError(native_error, "Metal binary archive add failed");
    return false;
  }
  std::lock_guard lock(mutex_);
  archived_.insert(recipe_hash);
  dirty_ = true;
  return true;
}

void PipelineStore::Skip(uint64_t recipe_hash) {
  std::lock_guard lock(mutex_);
  archived_.insert(recipe_hash);
  dirty_ = true;
}

bool PipelineStore::Save(std::string& error) {
  std::lock_guard lock(mutex_);
  if (!archive_ || !dirty_) return true;
  // Serialize beside the live archive, then swap, so an interrupted save never truncates it.
  const auto temporary = archive_path_.string() + ".tmp";
  NSError* native_error = nil;
  if (![archive_ serializeToURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:temporary.c_str()]]
                          error:&native_error]) {
    error = ui::metal::MetalError(native_error, "Metal binary archive serialization failed");
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(temporary, archive_path_, ec);
  if (ec) { error = "Cannot replace Metal binary archive: " + ec.message(); return false; }
  std::ofstream manifest(manifest_path_, std::ios::binary | std::ios::trunc);
  for (uint64_t hash : archived_) manifest.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
  dirty_ = false;
  ClearRebuildMarker();
  return true;
}

}  // namespace rex::graphics::gta4_metal
