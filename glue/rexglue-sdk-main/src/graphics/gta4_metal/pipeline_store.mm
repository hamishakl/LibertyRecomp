#include "pipeline_store.h"

#include <cstring>
#include <rex/logging.h>
#include <xxhash.h>
#include "../../ui/metal/context.h"

namespace rex::graphics::gta4_metal {

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
  char title[16];
  std::snprintf(title, sizeof(title), "%08X", title_id);
  directory_ = cache_root / "gta4-metal" / title;
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
  return true;
}

}  // namespace rex::graphics::gta4_metal
