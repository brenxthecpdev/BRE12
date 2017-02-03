#include <ShaderUtils/CBuffers.hlsli>
#include <ShaderUtils/Utils.hlsli>

#include "RS.hlsl"

#define SAMPLE_KERNEL_SIZE 128U
#define SCREEN_WIDTH 1920.0f
#define SCREEN_HEIGHT 1080.0f
#define NOISE_TEXTURE_DIMENSION 4.0f
#define NOISE_SCALE float2(SCREEN_WIDTH / NOISE_TEXTURE_DIMENSION, SCREEN_HEIGHT/ NOISE_TEXTURE_DIMENSION)
#define OCCLUSION_RADIUS 1.5f
#define SSAO_POWER 2.5f

//#define SKIP_AMBIENT_OCCLUSION

struct Input {
	float4 mPositionNDC : SV_POSITION;
	float3 mRayViewSpace : VIEW_RAY;
	float2 mUV : TEXCOORD;
};

ConstantBuffer<FrameCBuffer> gFrameCBuffer : register(b0);

SamplerState TextureSampler : register (s0);

Texture2D<float4> Normal_SmoothnessTexture : register (t0);
Texture2D<float> DepthTexture : register (t1);
StructuredBuffer<float4> RandomSamplesTexture : register(t2);
Texture2D<float4> NoiseTexture : register (t3); 

struct Output {
	float mAmbientAccessibility : SV_Target0;
};

[RootSignature(RS)]
Output main(const in Input input) {
	Output output = (Output)0;

#ifdef SKIP_AMBIENT_OCCLUSION
	output.mAmbientAccessibility = 1.0f;
#else
	const int3 fragmentScreenSpace = int3(input.mPositionNDC.xy, 0);

	const float depthNDC = DepthTexture.Load(fragmentScreenSpace);
	const float3 rayViewSpace = normalize(input.mRayViewSpace);
	const float4 fragmentPositionViewSpace = float4(ViewRayToViewPosition(rayViewSpace, depthNDC, gFrameCBuffer.mProjectionMatrix), 1.0f);

	const float2 normal = Normal_SmoothnessTexture.Load(fragmentScreenSpace).xy;
	const float3 normalViewSpace = normalize(Decode(normal));

	// Build a matrix to reorient the sample kernel
	// along current fragment normal vector.
	const float3 noiseVec = NoiseTexture.SampleLevel(TextureSampler, NOISE_SCALE * input.mUV, 0.0f).xyz * 2.0f - 1.0f;
	const float3 tangentViewSpace = normalize(noiseVec - normalViewSpace * dot(noiseVec, normalViewSpace));
	const float3 bitangentViewSpace = normalize(cross(normalViewSpace, tangentViewSpace));
	const float3x3 reorientationMatrix = float3x3(tangentViewSpace, bitangentViewSpace, normalViewSpace);

	float occlusionSum = 0.0f;
	for (uint i = 0U; i < SAMPLE_KERNEL_SIZE; ++i) {
		// Reorient random sample and get sample position in view space
		const float4 randomSample = float4(mul(RandomSamplesTexture[i].xyz, reorientationMatrix), 0.0f);
		float4 samplePositionViewSpace = fragmentPositionViewSpace + randomSample * OCCLUSION_RADIUS;
				
		float4 samplePositionNDC = mul(samplePositionViewSpace, gFrameCBuffer.mProjectionMatrix);
		samplePositionNDC.xy /= samplePositionNDC.w;
	
		const int2 samplePositionScreenSpace = NdcToScreenCoordinates(samplePositionNDC.xy, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT);

		float sampleDepthNDC = DepthTexture.Load(int3(samplePositionScreenSpace, 0));

		const float sampleDepthViewSpace = NdcDepthToViewDepth(sampleDepthNDC, gFrameCBuffer.mProjectionMatrix);

		occlusionSum += (sampleDepthViewSpace <= samplePositionViewSpace.z ? 1.0 : 0.0);
	}

	output.mAmbientAccessibility = 1.0f - (occlusionSum / SAMPLE_KERNEL_SIZE);
#endif

	// Sharpen the contrast
	output.mAmbientAccessibility = saturate(pow(output.mAmbientAccessibility, SSAO_POWER));

	return output;
}