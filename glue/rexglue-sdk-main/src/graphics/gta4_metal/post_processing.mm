#include "post_processing.h"
#include "gpu_pass_timer.h"
#include "../../ui/metal/context.h"
#include <postfx/smaa/AreaTex.h>
#include <postfx/smaa/SearchTex.h>
#include <algorithm>

namespace rex::graphics::gta4_metal {
bool PostProcessing::Initialize(std::shared_ptr<ui::metal::MetalContext> context, std::string& error) {
  @autoreleasepool {
    if (context_) return true;
    if (!context || !context->device || !context->pass_library || !transfer_.Initialize(context, error)) return false;
    context_ = std::move(context);error.clear();return true;
  }
}

bool PostProcessing::EnsureSmaa(size_t quality,std::string& error){
  @autoreleasepool {
    if(!context_||quality>=pipelines_.size()){error="Invalid SMAA quality";return false;}
    auto vertex = [context_->ui_library newFunctionWithName:@"liberty_present_vertex"];
    constexpr const char* qualities[] = {"low", "medium", "high", "ultra"};
    constexpr const char* stages[] = {"edge", "weight", "neighborhood"};
    constexpr MTLPixelFormat formats[] = {MTLPixelFormatRG8Unorm, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA16Float};
    if (!pipelines_[quality][0] || !pipelines_[quality][1] || !pipelines_[quality][2]) {
      for (size_t pass = 0; pass < pipelines_[quality].size(); ++pass) {
        auto descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        NSString* name = [NSString stringWithFormat:@"liberty_smaa_%s_ps_%s", stages[pass], qualities[quality]];
        descriptor.fragmentFunction = [context_->pass_library newFunctionWithName:name];
        descriptor.colorAttachments[0].pixelFormat = formats[pass];
        descriptor.label = name;
        if (!vertex || !descriptor.fragmentFunction) { error = "Missing SMAA shader"; return false; }
        NSError* native_error = nil;
        pipelines_[quality][pass] = [context_->device newRenderPipelineStateWithDescriptor:descriptor error:&native_error];
        if (!pipelines_[quality][pass]) { error = ui::metal::MetalError(native_error, "SMAA pipeline creation failed"); return false; }
      }
    }
    if(area_&&search_&&linear_&&point_){error.clear();return true;}
    auto sampler = [MTLSamplerDescriptor new];
    sampler.minFilter = sampler.magFilter = MTLSamplerMinMagFilterNearest;
    sampler.sAddressMode = sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
    point_ = [context_->device newSamplerStateWithDescriptor:sampler];
    sampler.minFilter = sampler.magFilter = MTLSamplerMinMagFilterLinear;
    linear_ = [context_->device newSamplerStateWithDescriptor:sampler];
    const auto lookup = [&](MTLPixelFormat format, NSUInteger width, NSUInteger height,
                            const void* bytes, NSUInteger pitch) -> id<MTLTexture> {
      auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
      descriptor.storageMode = MTLStorageModeShared;
      descriptor.usage = MTLTextureUsageShaderRead;
      auto image = [context_->device newTextureWithDescriptor:descriptor];
      if (image) [image replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:bytes bytesPerRow:pitch];
      return image;
    };
    static_assert(sizeof(areaTexBytes) == AREATEX_SIZE && sizeof(searchTexBytes) == SEARCHTEX_SIZE);
    area_ = lookup(MTLPixelFormatRG8Unorm, AREATEX_WIDTH, AREATEX_HEIGHT, areaTexBytes, AREATEX_PITCH);
    search_ = lookup(MTLPixelFormatR8Unorm, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, searchTexBytes, SEARCHTEX_PITCH);
    if (!area_ || !search_ || !linear_ || !point_) { error = "SMAA lookup resources unavailable"; return false; }
    area_.label = @"Liberty SMAA area lookup";
    search_.label = @"Liberty SMAA search lookup";
    error.clear();return true;
  }
}

bool PostProcessing::EnsureExtent(NSUInteger width, NSUInteger height, std::string& error) {
  if (images_[0] && images_[0].width == width && images_[0].height == height) return true;
  // Bound live replacement storage; queued commands retain older extents until done.
  if (!width || !height || width > 16384 || height > 16384 || uint64_t(width) * height * 14 > 512ull * 1024 * 1024) {
    error = "SMAA extent exceeds its allocation budget"; return false;
  }
  constexpr MTLPixelFormat formats[] = {MTLPixelFormatRG8Unorm, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA16Float};
  std::array<id<MTLTexture>, 3> next{};
  for (size_t i = 0; i < next.size(); ++i) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[i] width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    next[i] = [context_->device newTextureWithDescriptor:descriptor];
    if (!next[i]) { error = "SMAA intermediate allocation failed"; return false; }
    next[i].label = [NSString stringWithFormat:@"Liberty SMAA intermediate %zu", i];
  }
  images_ = next;
  return true;
}

bool PostProcessing::Record(id<MTLCommandBuffer> commands, id<MTLTexture> source,
    id<MTLTexture> destination, gta4_native::AntiAliasingMode mode, std::string_view quality,
    bool dither, std::string& error) {
  @autoreleasepool {
    error.clear();
    if (!context_ || !commands || !source || !destination || source == destination ||
        source.textureType != MTLTextureType2D || destination.textureType != MTLTextureType2D) {
      error = "Invalid post-processing source or destination"; return false;
    }
    const auto route = gta4_native::GetAntiAliasingRoute(mode);
    if (route.presentation_smaa) {
      const size_t q = quality == "low" ? 0 : quality == "medium" ? 1 : quality == "ultra" ? 3 : 2;
      if(!EnsureSmaa(q,error)||!EnsureExtent(source.width,source.height,error))return false;
      const std::array<float, 4> constants{1.0f / float(source.width), 1.0f / float(source.height),
                                          float(source.width), float(source.height)};
      const std::array<std::array<id<MTLTexture>, 3>, 3> textures{{
          {source, nil, nil}, {images_[0], area_, search_}, {source, images_[1], nil}}};
      const std::array<std::array<id<MTLSamplerState>, 3>, 3> samplers{{
          {point_, nil, nil}, {linear_, linear_, point_}, {linear_, linear_, nil}}};
      constexpr NSUInteger counts[] = {1, 3, 2};
      for (size_t i = 0; i < images_.size(); ++i) {
        auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = images_[i];
        // Edge detection discards non-edges; zero is meaningful input to weights.
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        gpu_pass_timer::Tag(pass,"post-processing");
        auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (!encoder) { error = "SMAA encoder creation failed"; return false; }
        encoder.label = [NSString stringWithFormat:@"Liberty SMAA pass %zu", i];
        [encoder setRenderPipelineState:pipelines_[q][i]];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setViewport:MTLViewport{0, 0, double(source.width), double(source.height), 0, 1}];
        [encoder setFragmentTextures:textures[i].data() withRange:NSMakeRange(0, counts[i])];
        [encoder setFragmentSamplerStates:samplers[i].data() withRange:NSMakeRange(0, counts[i])];
        [encoder setFragmentBytes:constants.data() length:sizeof(constants) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
      }
      source = images_[2];
    } else {
      ReleaseExtentResources();
    }
    ui::metal::OutputTransferConstants constants{};
    constants.output_mode = 4u | (route.presentation_fxaa ? 16u : 0u) | (dither ? 8u : 0u);
    if (route.supersampling_pixel_factor > 1 &&
        (source.width != destination.width || source.height != destination.height)) constants.output_mode |= 32u;
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = destination;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    gpu_pass_timer::Tag(pass,"post-processing");
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) { error = "Output-transfer encoder creation failed"; return false; }
    encoder.label = @"Liberty final title image";
    const bool success = transfer_.Draw(encoder, source, destination, constants, error);
    [encoder endEncoding];
    return success;
  }
}

void PostProcessing::ReleaseExtentResources() { images_ = {}; }
size_t PostProcessing::allocated_bytes() const {
  size_t bytes = area_.allocatedSize + search_.allocatedSize;
  for (auto image : images_) bytes += image.allocatedSize;
  return bytes;
}
}  // namespace rex::graphics::gta4_metal
