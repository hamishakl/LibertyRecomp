#include "renderer_state.h"
#include "override_metadata.h"
#include "../gta4_native/native_fixed_function_policy.h"
#include "../gta4_native/alpha_to_coverage_util.h"
#include <algorithm>
#include <chrono>
#include <optional>
#include <rex/logging.h>
#include <rex/cvar.h>

namespace rex::graphics::gta4_metal {
namespace {
constexpr uint64_t kBlackGammaCompatibilityPixelShader = 0xF6AEB9A606561C54ull;

std::optional<MTLBlendFactor> BlendFactor(uint32_t value) {
  switch(value) {
    case 0:return MTLBlendFactorZero; case 1:return MTLBlendFactorOne;
    case 4:return MTLBlendFactorSourceColor; case 5:return MTLBlendFactorOneMinusSourceColor;
    case 6:return MTLBlendFactorSourceAlpha; case 7:return MTLBlendFactorOneMinusSourceAlpha;
    case 8:return MTLBlendFactorDestinationColor; case 9:return MTLBlendFactorOneMinusDestinationColor;
    case 10:return MTLBlendFactorDestinationAlpha; case 11:return MTLBlendFactorOneMinusDestinationAlpha;
    case 12:return MTLBlendFactorBlendColor; case 13:return MTLBlendFactorOneMinusBlendColor;
    case 14:return MTLBlendFactorBlendAlpha; case 15:return MTLBlendFactorOneMinusBlendAlpha;
    case 16:return MTLBlendFactorSourceAlphaSaturated; default:return {};
  }
}
std::optional<MTLBlendOperation> BlendOperation(uint32_t value) {
  switch(value) {
    case 0:return MTLBlendOperationAdd; case 1:return MTLBlendOperationSubtract;
    case 2:return MTLBlendOperationMin; case 3:return MTLBlendOperationMax;
    case 4:return MTLBlendOperationReverseSubtract; default:return {};
  }
}
MTLColorWriteMask ColorMask(uint32_t value) {
  MTLColorWriteMask result=MTLColorWriteMaskNone;
  if(value&1) result|=MTLColorWriteMaskRed;
  if(value&2) result|=MTLColorWriteMaskGreen;
  if(value&4) result|=MTLColorWriteMaskBlue;
  if(value&8) result|=MTLColorWriteMaskAlpha;
  return result;
}
MTLVertexFormat VertexFormat(uint32_t type,MetalVertexScalar scalar) {
  const bool u=scalar==MetalVertexScalar::kUnsignedInteger;
  const bool i=scalar==MetalVertexScalar::kSignedInteger;
  switch(type) {
    case 0x2C83A4:return !u&&!i?MTLVertexFormatFloat:MTLVertexFormatInvalid;
    case 0x2C23A5:return !u&&!i?MTLVertexFormatFloat2:MTLVertexFormatInvalid;
    case 0x2A23B9:return !u&&!i?MTLVertexFormatFloat3:MTLVertexFormatInvalid;
    case 0x1A23A6:return !u&&!i?MTLVertexFormatFloat4:MTLVertexFormatInvalid;
    case 0x182886:return u?MTLVertexFormatUChar4:MTLVertexFormatUChar4Normalized_BGRA;
    case 0x1A2286:case 0x1A2386:return MTLVertexFormatUChar4;
    case 0x2C2359:return MTLVertexFormatShort2;
    case 0x1A235A:return MTLVertexFormatShort4;
    case 0x1A2086:case 0x1A2186:return u?MTLVertexFormatUChar4:MTLVertexFormatUChar4Normalized;
    case 0x2C2159:return i?MTLVertexFormatShort2:MTLVertexFormatShort2Normalized;
    case 0x1A215A:return i?MTLVertexFormatShort4:MTLVertexFormatShort4Normalized;
    case 0x2C2059:return u?MTLVertexFormatUShort2:MTLVertexFormatUShort2Normalized;
    case 0x1A205A:return u?MTLVertexFormatUShort4:MTLVertexFormatUShort4Normalized;
    case 0x2C82A1:case 0x2A2287:case 0x2A2187:case 0x2A2190:case 0x2A2390:return MTLVertexFormatUInt;
    case 0x1A2187:return MTLVertexFormatInt1010102Normalized;
    case 0x2C235F:return MTLVertexFormatHalf2;
    case 0x1A2360:return MTLVertexFormatHalf4;
    default:return MTLVertexFormatInvalid;
  }
}
MTLVertexFormat DefaultVertexFormat(const MetalVertexAttribute& attribute) {
  const MTLVertexFormat values[][4]={{MTLVertexFormatFloat,MTLVertexFormatFloat2,MTLVertexFormatFloat3,MTLVertexFormatFloat4},
      {MTLVertexFormatInt,MTLVertexFormatInt2,MTLVertexFormatInt3,MTLVertexFormatInt4},
      {MTLVertexFormatUInt,MTLVertexFormatUInt2,MTLVertexFormatUInt3,MTLVertexFormatUInt4}};
  if(uint32_t(attribute.scalar_type)>2 || !attribute.components || attribute.components>4) return MTLVertexFormatInvalid;
  return values[uint32_t(attribute.scalar_type)][attribute.components-1];
}
}

Renderer::State::Pipeline* Renderer::State::DrawPipeline(const Targets& targets,const FixedState& fixed,
    const VertexDeclaration& vdecl,bool up,uint32_t up_stride,std::string& error) {
  using namespace gta4_native;
  const auto vertex=shaders.find(vertex_shader),pixel=shaders.find(pixel_shader);
  if(vertex==shaders.end() || vertex->second.stage!=ShaderStage::kVertex ||
      (pixel_shader && (pixel==shaders.end() || pixel->second.stage!=ShaderStage::kPixel))) {
    error="Draw references an unregistered shader"; return nullptr;
  }
  auto selection=ResolveModernShaderSelection(override_mode,modern.settings(),
      FindOverrideCandidate(vertex->second.hash),FindOverrideCandidate(pixel_shader?pixel->second.hash:0),targets.samples);
  // This stock composite's gamma factorization turns finite black into NaN.
  // Its exact-zero compatibility repair is required even in stock visual mode.
  if(pixel_shader && pixel->second.hash==kBlackGammaCompatibilityPixelShader) {
    selection.pixel_override=true;
    selection.variant_key=MakeShaderOverrideVariantKey(
        selection.vertex_override,selection.pixel_override,selection.pipeline_pair_id);
  }
  const auto* vs=selection.vertex_override?overrides.Metadata(vertex->second.hash,ShaderStage::kVertex,error):&vertex->second;
  const ShaderMetadata* ps=nullptr;
  if(pixel_shader)ps=selection.pixel_override?overrides.Metadata(pixel->second.hash,ShaderStage::kPixel,error):&pixel->second;
  if(!vs || (pixel_shader && !ps)) return nullptr;
  const bool alpha=fixed.alpha_test_enable && fixed.alpha_function!=7;
  const bool coverage=alpha || IsNativeAlphaToMaskRequested(fixed.alpha_to_mask);
  const bool late=coverage && ps && ps->late_available;
  if(coverage && (!ps || !late || (ps->specialization_constants_mask&0x702u)!=0x702u)) {
    error="Draw requires an unavailable late-test coverage shader"; return nullptr;
  }
  const uint32_t specialization=alpha ? 2u|((fixed.alpha_function&7u)<<8u) : 0;
  Key key{}; size_t cursor=0;
  auto add=[&](uint64_t value) {key.words[cursor++]=value;};
  add(vs->hash); add(ps?ps->hash:0); add(selection.variant_key); add(vdecl.identity);
  add(specialization); add(late); add(targets.samples); add(targets.depth?targets.depth->image.pixelFormat:0);
  add(up); add(fixed.color_write_mask);
  add(uint32_t(targets.temporal_motion)|(uint32_t(targets.temporal_reactive)<<1)|
      (uint32_t(targets.temporal_ui)<<2)|(uint32_t(targets.temporal_jitter)<<3));
  for(uint32_t i=0;i<kRenderTargetCount;++i) {
    add(targets.colors[i]?targets.colors[i]->image.pixelFormat:0);
    add(targets.colors[i]?fixed.blend_controls[i]:0);
  }
  for(uint32_t stream=0;stream<kVertexStreamCount;++stream) add(up?(stream==0?up_stride:0):streams[stream].stride);
  ++pipeline_lookups;
  if (cache_pipeline_lookup) {
    if (auto* cached = pipeline_lookup.Find(key)) {
      ++pipeline_lookup_hits;
      return cached;
    }
  }
  if (auto found = pipelines.find(key); found != pipelines.end()) {
    if (cache_pipeline_lookup) pipeline_lookup.Remember(key, &found->second);
    return &found->second;
  }
  Pipeline result; result.variant=selection.variant_key; result.vertex=*vs; result.has_pixel=bool(ps); if(ps) result.pixel=*ps;
  result.temporal_variant=targets.temporal_motion||targets.temporal_reactive||targets.temporal_ui||targets.temporal_jitter;
  auto& vertex_cache=result.temporal_variant?(selection.vertex_override?temporal_overrides:temporal_stock):(selection.vertex_override?overrides:stock);
  auto& pixel_cache=result.temporal_variant?(selection.pixel_override?temporal_overrides:temporal_stock):(selection.pixel_override?overrides:stock);
  auto descriptor=[MTLRenderPipelineDescriptor new];
  descriptor.vertexFunction=vertex_cache.Function(vs->hash,ShaderStage::kVertex,0,false,error);
  if(!descriptor.vertexFunction) return nullptr;
  if(ps) {
    descriptor.fragmentFunction=pixel_cache.Function(ps->hash,ShaderStage::kPixel,specialization,late,error,targets.temporal_ui);
    if(!descriptor.fragmentFunction) return nullptr;
  } else if(targets.temporal_motion) {
    descriptor.fragmentFunction=temporal_scene.DepthMotionFunction();
    if(!descriptor.fragmentFunction){error="Depth-only temporal motion fragment is absent";return nullptr;}
  }
  descriptor.rasterSampleCount=targets.samples;
  descriptor.alphaToCoverageEnabled=NO;
  descriptor.depthAttachmentPixelFormat=targets.depth?targets.depth->image.pixelFormat:MTLPixelFormatInvalid;
  descriptor.stencilAttachmentPixelFormat=descriptor.depthAttachmentPixelFormat;
  descriptor.label=[NSString stringWithFormat:@"Liberty %016llX/%016llX",vs->hash,ps?ps->hash:0];
  for(uint32_t i=0;i<kRenderTargetCount;++i) {
    if(!targets.colors[i]) continue;
    auto attachment=descriptor.colorAttachments[i];
    attachment.pixelFormat=targets.colors[i]->image.pixelFormat;
    const uint32_t write=(ps && (ps->color_output_mask&(1u<<i)))?NativeColorWriteMaskForTarget(fixed.color_write_mask,i):0;
    attachment.writeMask=ColorMask(write);
    attachment.blendingEnabled=IsNativeBlendControlEnabled(fixed.blend_controls[i]);
    if(!attachment.blendingEnabled) continue;
    const auto blend=DecodeNativeBlendControl(fixed.blend_controls[i]);
    const auto src=BlendFactor(blend.source_color),dst=BlendFactor(blend.destination_color);
    const auto src_a=BlendFactor(blend.source_alpha),dst_a=BlendFactor(blend.destination_alpha);
    const auto op=BlendOperation(blend.color_operation),op_a=BlendOperation(blend.alpha_operation);
    if(!src || !dst || !src_a || !dst_a || !op || !op_a) {error="Unsupported title blend state"; return nullptr;}
    attachment.sourceRGBBlendFactor=*src; attachment.destinationRGBBlendFactor=*dst;
    attachment.sourceAlphaBlendFactor=*src_a; attachment.destinationAlphaBlendFactor=*dst_a;
    attachment.rgbBlendOperation=*op; attachment.alphaBlendOperation=*op_a;
  }
  if(targets.temporal_motion||targets.temporal_reactive){
    descriptor.colorAttachments[4].pixelFormat=MTLPixelFormatRG16Float;
    descriptor.colorAttachments[4].writeMask=targets.temporal_motion?MTLColorWriteMaskAll:MTLColorWriteMaskNone;
    descriptor.colorAttachments[5].pixelFormat=MTLPixelFormatR8Unorm;
    if(targets.temporal_reactive){
      auto a=descriptor.colorAttachments[5];a.blendingEnabled=YES;a.rgbBlendOperation=MTLBlendOperationMax;
      a.sourceRGBBlendFactor=MTLBlendFactorOne;a.destinationRGBBlendFactor=MTLBlendFactorOne;
    }
  }
  if(targets.temporal_motion){
    auto a=descriptor.colorAttachments[6];a.pixelFormat=MTLPixelFormatR32Float;
    a.writeMask=MTLColorWriteMaskRed;a.blendingEnabled=NO;
  }
  if(targets.temporal_ui){
    auto a=descriptor.colorAttachments[6];a.pixelFormat=MTLPixelFormatRGBA16Float;a.writeMask=ColorMask(NativeColorWriteMaskForTarget(fixed.color_write_mask,0)&7u);
    const auto b=DecodeNativeBlendControl(fixed.blend_controls[0]);
    a.blendingEnabled=YES;a.sourceRGBBlendFactor=MTLBlendFactorOne;
    a.destinationRGBBlendFactor=IsNativeBlendControlEnabled(fixed.blend_controls[0])?BlendFactor(b.destination_color).value_or(MTLBlendFactorZero):MTLBlendFactorZero;
    if(b.source_color==10&&b.destination_color==11&&b.color_operation==0)
      a.destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
    auto t=descriptor.colorAttachments[7];t.pixelFormat=MTLPixelFormatRGBA16Float;t.writeMask=ColorMask(NativeColorWriteMaskForTarget(fixed.color_write_mask,0)&7u);
    t.blendingEnabled=YES;t.sourceRGBBlendFactor=MTLBlendFactorZero;t.destinationRGBBlendFactor=MTLBlendFactorSourceColor;
  }
  auto layout=[MTLVertexDescriptor vertexDescriptor];
  for(uint32_t index=0;index<vs->attribute_count;++index) {
    const auto& input=vs->attributes[index];
    auto element=std::find_if(vdecl.elements.begin(),vdecl.elements.end(),[&](const auto& e) {
      return VertexSemanticLocation(e.usage,e.usage_index)==input.semantic_location;
    });
    auto attribute=layout.attributes[input.index];
    if(element==vdecl.elements.end() || (up && element->stream!=0) ||
        (!up && !streams[element->stream].buffer)) {
      attribute.bufferIndex=30; attribute.format=DefaultVertexFormat(input); attribute.offset=0;
      layout.layouts[30].stride=16; layout.layouts[30].stepFunction=MTLVertexStepFunctionPerInstance;
    } else {
      const uint32_t stride=up?up_stride:streams[element->stream].stride;
      if(!stride || stride>2048 || element->offset>=stride || element->stream>=kVertexStreamCount) {
        error="Invalid vertex attribute extent/stride"; return nullptr;
      }
      attribute.bufferIndex=9+element->stream; attribute.offset=element->offset;
      attribute.format=VertexFormat(element->type,input.scalar_type);
      layout.layouts[attribute.bufferIndex].stride=stride;
      layout.layouts[attribute.bufferIndex].stepFunction=MTLVertexStepFunctionPerVertex;
      result.strides[element->stream]=stride;
    }
    if(attribute.format==MTLVertexFormatInvalid) {error="Unsupported Metal vertex format"; return nullptr;}
  }
  descriptor.vertexDescriptor=layout;
  if (pipeline_store.is_open() && use_pipeline_archive) {
    RecipeFunction vertex_ref{vs->hash,0,LibraryOf(vertex_cache),uint8_t(ShaderStage::kVertex),0,0};
    RecipeFunction fragment_ref{};
    if(ps) fragment_ref={ps->hash,specialization,LibraryOf(pixel_cache),uint8_t(ShaderStage::kPixel),
                         uint8_t(late),uint8_t(targets.temporal_ui)};
    else if(targets.temporal_motion) fragment_ref.library=RecipeLibrary::kDepthMotion;
    pipeline_store.Record(RecipeFromDescriptor(descriptor,vertex_ref,fragment_ref));
    // Archived pipelines are fetched instead of compiled (see launch precompile).
    descriptor.binaryArchives=@[pipeline_store.archive()];
  }
  if (rex::cvar::Query<bool>("gta4_metal_async_pipelines")) {
    auto build = std::make_shared<PipelineBuild>(); result.build = build;
    // The callback owns its result, never a renderer/map pointer. Eviction and
    // shutdown can safely outlive an outstanding compiler callback.
    [context->device newRenderPipelineStateWithDescriptor:descriptor
        completionHandler:^(id<MTLRenderPipelineState> state, NSError* native_error) {
      { std::lock_guard lock(build->mutex);
        build->state = state;
        if (!state) build->error = ui::metal::MetalError(native_error, "Metal title pipeline creation failed");
        build->complete = true;
      }
      build->wake.notify_all();
    }];
  } else {
    NSError* native_error = nil;
    result.state = [context->device newRenderPipelineStateWithDescriptor:descriptor error:&native_error];
    if (!result.state) { error = ui::metal::MetalError(native_error, "Metal title pipeline creation failed"); return nullptr; }
  }
  ++pipeline_creations;
  if(profile_enabled) REXLOG_INFO("gta4-metal-profile-shader vs={:016X} ps={:016X} variant={:X} selected-vs={} selected-ps={}",
      vs->hash,ps?ps->hash:0,selection.variant_key,vs->name,ps?ps->name:"none");
  if (pipelines.size() >= 4096) {
    pipeline_lookup.Reset();  // Invalidate borrowed pointers before eviction.
    pipelines.erase(pipelines.begin());
  }
  auto* pipeline = &pipelines.emplace(key, std::move(result)).first->second;
  if (cache_pipeline_lookup) pipeline_lookup.Remember(key, pipeline);
  return pipeline;
}

id<MTLFunction> Renderer::State::RecipeFunctionObject(const RecipeFunction& function, std::string& error) {
  using namespace gta4_native;
  if(function.library==RecipeLibrary::kNone) return nil;
  if(function.library==RecipeLibrary::kDepthMotion) {
    auto result=temporal_scene.DepthMotionFunction();
    if(!result) error="Depth-only temporal motion fragment is absent";
    return result;
  }
  auto* cache=CacheOf(function.library);
  if(!cache){error="Unknown recipe shader library";return nil;}
  // Temporal archives are only loaded when a temporal mode is active.
  return cache->Function(function.hash,ShaderStage(function.stage),function.specialization,function.late!=0,
                         error,function.isolated_ui!=0);
}

bool Renderer::State::CompletePipeline(Pipeline& pipeline, std::string& error) {
  if (pipeline.state) return true;
  auto build = pipeline.build;
  if (!build) { error = "Missing Metal pipeline build"; return false; }
  {
    std::unique_lock lock(build->mutex);
    if (build->complete) ++pipeline_ready;
    else {
      ++pipeline_waits;
      const auto begin = std::chrono::steady_clock::now();
      build->wake.wait(lock, [&] { return build->complete; });
      pipeline_wait_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - begin).count());
    }
    pipeline.state = build->state;
    if (!pipeline.state) { error = build->error; return false; }
  }
  pipeline.build.reset(); return true;
}

id<MTLDepthStencilState> Renderer::State::DepthState(const FixedState& fixed,bool attachment,std::string& error) {
  Key key{};
  key.words[0]=attachment&&fixed.depth_enable;
  key.words[1]=key.words[0]?fixed.depth_function:7;
  key.words[2]=key.words[0]&&fixed.depth_write_enable;
  key.words[3]=attachment&&fixed.stencil_enable;
  if(key.words[3]) {
    key.words[4]=fixed.stencil_function; key.words[5]=fixed.stencil_fail;
    key.words[6]=fixed.stencil_depth_fail; key.words[7]=fixed.stencil_pass;
    key.words[8]=fixed.stencil_mask; key.words[9]=fixed.stencil_write_mask;
    key.words[10]=fixed.two_sided_stencil?fixed.ccw_stencil_function:fixed.stencil_function;
    key.words[11]=fixed.two_sided_stencil?fixed.ccw_stencil_fail:fixed.stencil_fail;
    key.words[12]=fixed.two_sided_stencil?fixed.ccw_stencil_depth_fail:fixed.stencil_depth_fail;
    key.words[13]=fixed.two_sided_stencil?fixed.ccw_stencil_pass:fixed.stencil_pass;
    key.words[14]=fixed.two_sided_stencil?fixed.back_stencil_mask:fixed.stencil_mask;
    key.words[15]=fixed.two_sided_stencil?fixed.back_stencil_write_mask:fixed.stencil_write_mask;
  }
  if(auto found=depth_states.find(key);found!=depth_states.end()) return found->second;
  auto descriptor=[MTLDepthStencilDescriptor new];
  descriptor.depthCompareFunction=MTLCompareFunction(key.words[1]);
  descriptor.depthWriteEnabled=key.words[2];
  if(key.words[3]) {
    for(size_t face=0;face<2;++face) {
      const size_t begin=4+face*6;
      auto stencil=[MTLStencilDescriptor new];
      stencil.stencilCompareFunction=MTLCompareFunction(key.words[begin]);
      stencil.stencilFailureOperation=MTLStencilOperation(key.words[begin+1]);
      stencil.depthFailureOperation=MTLStencilOperation(key.words[begin+2]);
      stencil.depthStencilPassOperation=MTLStencilOperation(key.words[begin+3]);
      stencil.readMask=key.words[begin+4]; stencil.writeMask=key.words[begin+5];
      if(face==0) descriptor.frontFaceStencil=stencil; else descriptor.backFaceStencil=stencil;
    }
  }
  auto state=[context->device newDepthStencilStateWithDescriptor:descriptor];
  if(!state) {error="Metal depth/stencil state creation failed"; return nil;}
  if(depth_states.size()>=1024) depth_states.erase(depth_states.begin());
  depth_states.emplace(key,state); return state;
}
}  // namespace rex::graphics::gta4_metal
