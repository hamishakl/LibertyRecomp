// Exact XenosRecomp translation of GTA IV's PS_Bink container
// (cache hash 0x9E76B68B60127349). The late-testing build removes only the
// early-fragment-tests execution mode required by alpha-test specialization.
#include "../xenos_shader_common.h"

#ifdef __spirv__

#define APlaneSampler_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 12)
#define APlaneSampler_Texture2DArrayDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 116)
#define APlaneSampler_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 220)
#define APlaneSampler_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 324)
#define APlaneSampler_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 428)
#define YPlaneSampler_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0)
#define YPlaneSampler_Texture2DArrayDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 104)
#define YPlaneSampler_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 208)
#define YPlaneSampler_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 312)
#define YPlaneSampler_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 416)
#define YUVtoRGB(INDEX) selectWrapper((INDEX) < 16, vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + (208 + min(INDEX, 15)) * 16, 0x10), 0.0)
#define cBPlaneSampler_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 8)
#define cBPlaneSampler_Texture2DArrayDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 112)
#define cBPlaneSampler_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 216)
#define cBPlaneSampler_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 320)
#define cBPlaneSampler_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 424)
#define cRPlaneSampler_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 4)
#define cRPlaneSampler_Texture2DArrayDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 108)
#define cRPlaneSampler_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 212)
#define cRPlaneSampler_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 316)
#define cRPlaneSampler_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 420)

#elif defined(__air__)

#define APlaneSampler_Texture2DDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 12)))
#define APlaneSampler_Texture2DArrayDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 116)))
#define APlaneSampler_Texture3DDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 220)))
#define APlaneSampler_TextureCubeDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 324)))
#define APlaneSampler_SamplerDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 428)))
#define YPlaneSampler_Texture2DDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 0)))
#define YPlaneSampler_Texture2DArrayDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 104)))
#define YPlaneSampler_Texture3DDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 208)))
#define YPlaneSampler_TextureCubeDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 312)))
#define YPlaneSampler_SamplerDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 416)))
#define YUVtoRGB(INDEX) selectWrapper((INDEX) < 16, (*(reinterpret_cast<device float4*>(g_PushConstants.PixelShaderConstants + (208 + min((uint)(INDEX), (uint)15)) * 16))), 0.0)
#define cBPlaneSampler_Texture2DDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 8)))
#define cBPlaneSampler_Texture2DArrayDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 112)))
#define cBPlaneSampler_Texture3DDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 216)))
#define cBPlaneSampler_TextureCubeDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 320)))
#define cBPlaneSampler_SamplerDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 424)))
#define cRPlaneSampler_Texture2DDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 4)))
#define cRPlaneSampler_Texture2DArrayDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 108)))
#define cRPlaneSampler_Texture3DDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 212)))
#define cRPlaneSampler_TextureCubeDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 316)))
#define cRPlaneSampler_SamplerDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 420)))

#else

cbuffer PixelShaderConstants : register(b1, space4)
{
	float4 YUVtoRGB[4] : packoffset(c208);
#define YUVtoRGB(INDEX) selectWrapper((INDEX) < 16, YUVtoRGB[min(INDEX, 15)], 0.0)
};

cbuffer SharedConstants : register(b2, space4)
{
	uint APlaneSampler_Texture2DDescriptorIndex : packoffset(c0.w);
	uint APlaneSampler_Texture2DArrayDescriptorIndex : packoffset(c7.y);
	uint APlaneSampler_Texture3DDescriptorIndex : packoffset(c13.w);
	uint APlaneSampler_TextureCubeDescriptorIndex : packoffset(c20.y);
	uint APlaneSampler_SamplerDescriptorIndex : packoffset(c26.w);
	uint YPlaneSampler_Texture2DDescriptorIndex : packoffset(c0.x);
	uint YPlaneSampler_Texture2DArrayDescriptorIndex : packoffset(c6.z);
	uint YPlaneSampler_Texture3DDescriptorIndex : packoffset(c13.x);
	uint YPlaneSampler_TextureCubeDescriptorIndex : packoffset(c19.z);
	uint YPlaneSampler_SamplerDescriptorIndex : packoffset(c26.x);
	uint cBPlaneSampler_Texture2DDescriptorIndex : packoffset(c0.z);
	uint cBPlaneSampler_Texture2DArrayDescriptorIndex : packoffset(c7.x);
	uint cBPlaneSampler_Texture3DDescriptorIndex : packoffset(c13.z);
	uint cBPlaneSampler_TextureCubeDescriptorIndex : packoffset(c20.x);
	uint cBPlaneSampler_SamplerDescriptorIndex : packoffset(c26.z);
	uint cRPlaneSampler_Texture2DDescriptorIndex : packoffset(c0.y);
	uint cRPlaneSampler_Texture2DArrayDescriptorIndex : packoffset(c6.w);
	uint cRPlaneSampler_Texture3DDescriptorIndex : packoffset(c13.y);
	uint cRPlaneSampler_TextureCubeDescriptorIndex : packoffset(c19.w);
	uint cRPlaneSampler_SamplerDescriptorIndex : packoffset(c26.y);
	DEFINE_SHARED_CONSTANTS();
};

#endif

struct Interpolators
{
#ifdef __air__
	float4 iPos [[position]];
	float4 iTexCoord0 [[user(TEXCOORD0)]];
	float4 iTexCoord1 [[user(TEXCOORD1)]];
	float4 iTexCoord2 [[user(TEXCOORD2)]];
	float4 iTexCoord3 [[user(TEXCOORD3)]];
	float4 iTexCoord4 [[user(TEXCOORD4)]];
	float4 iTexCoord5 [[user(TEXCOORD5)]];
	float4 iTexCoord6 [[user(TEXCOORD6)]];
	float4 iTexCoord7 [[user(TEXCOORD7)]];
	float4 iTexCoord8 [[user(TEXCOORD8)]];
	float4 iTexCoord9 [[user(TEXCOORD9)]];
	float4 iTexCoord10 [[user(TEXCOORD10)]];
	float4 iTexCoord11 [[user(TEXCOORD11)]];
	float4 iTexCoord12 [[user(TEXCOORD12)]];
	float4 iTexCoord13 [[user(TEXCOORD13)]];
	float4 iTexCoord14 [[user(TEXCOORD14)]];
	float4 iTexCoord15 [[user(TEXCOORD15)]];
	float4 iColor0 [[user(COLOR0)]];
	float4 iColor1 [[user(COLOR1)]];
#else
	float4 iPos : SV_Position;
	float4 iTexCoord0 : TEXCOORD0;
	float4 iTexCoord1 : TEXCOORD1;
	float4 iTexCoord2 : TEXCOORD2;
	float4 iTexCoord3 : TEXCOORD3;
	float4 iTexCoord4 : TEXCOORD4;
	float4 iTexCoord5 : TEXCOORD5;
	float4 iTexCoord6 : TEXCOORD6;
	float4 iTexCoord7 : TEXCOORD7;
	float4 iTexCoord8 : TEXCOORD8;
	float4 iTexCoord9 : TEXCOORD9;
	float4 iTexCoord10 : TEXCOORD10;
	float4 iTexCoord11 : TEXCOORD11;
	float4 iTexCoord12 : TEXCOORD12;
	float4 iTexCoord13 : TEXCOORD13;
	float4 iTexCoord14 : TEXCOORD14;
	float4 iTexCoord15 : TEXCOORD15;
	float4 iColor0 : COLOR0;
	float4 iColor1 : COLOR1;
#endif
};
struct PixelShaderOutput
{
#ifdef __air__
	float4 oC0 [[color(0)]];
#else
	float4 oC0 : SV_Target0;
#endif
};
#ifdef __air__
[[fragment]]
[[early_fragment_tests]]
#else
#if !defined(__spirv__)
[shader("pixel")]
#endif
#ifndef XENOS_RECOMP_LATE_FRAGMENT_TESTS
[earlydepthstencil]
#endif
#endif
PixelShaderOutput shaderMain(
#ifdef __air__
	Interpolators input [[stage_in]],
	bool iFace [[front_facing]],
	constant Texture2DDescriptorHeap* g_Texture2DDescriptorHeap [[buffer(0)]],
	constant Texture2DArrayDescriptorHeap* g_Texture2DArrayDescriptorHeap [[buffer(1)]],
	constant Texture3DDescriptorHeap* g_Texture3DDescriptorHeap [[buffer(2)]],
	constant TextureCubeDescriptorHeap* g_TextureCubeDescriptorHeap [[buffer(3)]],
	constant SamplerDescriptorHeap* g_SamplerDescriptorHeap [[buffer(4)]],
	constant PushConstants& g_PushConstants [[buffer(8)]]
#else
	Interpolators input,
#ifdef __spirv__
	in bool iFace : SV_IsFrontFace
#else
	in uint iFace : SV_IsFrontFace
#endif

#endif
)
{
#ifdef __air__
	PixelShaderOutput output = PixelShaderOutput{};
#else
	PixelShaderOutput output = (PixelShaderOutput)0;
#endif
#ifdef __air__
	float4 c252 = as_type<float4>(uint4(0x0, 0x0, 0x0, 0x0));
#else
	float4 c252 = asfloat(uint4(0x0, 0x0, 0x0, 0x0));
#endif
#ifdef __air__
	float4 c253 = as_type<float4>(uint4(0x0, 0x0, 0x0, 0x0));
#else
	float4 c253 = asfloat(uint4(0x0, 0x0, 0x0, 0x0));
#endif
#ifdef __air__
	float4 c254 = as_type<float4>(uint4(0x0, 0x0, 0x0, 0x0));
#else
	float4 c254 = asfloat(uint4(0x0, 0x0, 0x0, 0x0));
#endif
#ifdef __air__
	float4 c255 = as_type<float4>(uint4(0x0, 0x3F800000, 0x0, 0x0));
#else
	float4 c255 = asfloat(uint4(0x0, 0x3F800000, 0x0, 0x0));
#endif

	float4 r0 = input.iTexCoord0;
	float4 r1 = 0.0;
	float4 r2 = 0.0;
	float4 r3 = 0.0;
	float4 r4 = 0.0;
	float4 r5 = 0.0;
	float4 r6 = 0.0;
	float4 r7 = 0.0;
	float4 r8 = 0.0;
	float4 r9 = 0.0;
	float4 r10 = 0.0;
	float4 r11 = 0.0;
	float4 r12 = 0.0;
	float4 r13 = 0.0;
	float4 r14 = 0.0;
	float4 r15 = 0.0;
	float4 r16 = 0.0;
	float4 r17 = 0.0;
	float4 r18 = 0.0;
	float4 r19 = 0.0;
	float4 r20 = 0.0;
	float4 r21 = 0.0;
	float4 r22 = 0.0;
	float4 r23 = 0.0;
	float4 r24 = 0.0;
	float4 r25 = 0.0;
	float4 r26 = 0.0;
	float4 r27 = 0.0;
	float4 r28 = 0.0;
	float4 r29 = 0.0;
	float4 r30 = 0.0;
	float4 r31 = 0.0;
	int a0 = 0;
	int aL = 0;
	bool p0 = false;
	float ps = 0.0;

	r2.x = tfetch2D(
#ifdef __air__
		g_Texture2DDescriptorHeap,
		g_SamplerDescriptorHeap,
#endif
		YPlaneSampler_Texture2DDescriptorIndex, YPlaneSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0), GetTextureLodBias(0)).x;
	r2.y = tfetch2D(
#ifdef __air__
		g_Texture2DDescriptorHeap,
		g_SamplerDescriptorHeap,
#endif
		cBPlaneSampler_Texture2DDescriptorIndex, cBPlaneSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0), GetTextureLodBias(2)).x;
	r2.z = tfetch2D(
#ifdef __air__
		g_Texture2DDescriptorHeap,
		g_SamplerDescriptorHeap,
#endif
		cRPlaneSampler_Texture2DDescriptorIndex, cRPlaneSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0), GetTextureLodBias(1)).x;
	r0.x = tfetch2D(
#ifdef __air__
		g_Texture2DDescriptorHeap,
		g_SamplerDescriptorHeap,
#endif
		APlaneSampler_Texture2DDescriptorIndex, APlaneSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0), GetTextureLodBias(3)).x;
	r0.y = (float)((YUVtoRGB(0).w * YUVtoRGB(3).x));
	r1.x = (float)((dot(r2.yxz, YUVtoRGB(0).zxy)));
	ps = max(YUVtoRGB(2).w, YUVtoRGB(2).w);
	r1.y = (float)((dot(r2.yxz, YUVtoRGB(2).zxy)));
	ps = YUVtoRGB(3).x * ps;
	r0.z = ps;
	r2.xyzw = (float4)((selectWrapper(c255.xxxy == 0.0, r2.yxzz, YUVtoRGB(3).xxxx)));
	output.oC0.y = (float)((dot(r2.xyzw, YUVtoRGB(1).zxyw)));
	output.oC0.xz = (float2)((r1.xy + r0.yz));
	ps = YUVtoRGB(3).w * r0.x;
	output.oC0.w = ps;
	BRANCH if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)
	{
		uint alphaTestFunction = (g_SpecConstants() >> SPEC_CONSTANT_ALPHA_TEST_FUNCTION_SHIFT) & 7u;
		bool alphaTestPass;
		if (alphaTestFunction == 0u) alphaTestPass = false;
		else if (alphaTestFunction == 1u) alphaTestPass = output.oC0.w < g_AlphaThreshold;
		else if (alphaTestFunction == 2u) alphaTestPass = output.oC0.w == g_AlphaThreshold;
		else if (alphaTestFunction == 3u) alphaTestPass = output.oC0.w <= g_AlphaThreshold;
		else if (alphaTestFunction == 4u) alphaTestPass = output.oC0.w > g_AlphaThreshold;
		else if (alphaTestFunction == 5u) alphaTestPass = any(isnan(float2(output.oC0.w, g_AlphaThreshold))) || output.oC0.w != g_AlphaThreshold;
		else if (alphaTestFunction == 6u) alphaTestPass = output.oC0.w >= g_AlphaThreshold;
		else alphaTestPass = true;
		clip(alphaTestPass ? 1.0 : -1.0);
	}
	return output;
}
