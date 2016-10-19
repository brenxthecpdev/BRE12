#include <ShaderUtils/CBuffers.hlsli>

struct Input {
	float3 mPosH : POSITION;
	float3 mNormalO : NORMAL;
	float3 mTangentO : TANGENT;
	float2 mTexCoordO : TEXCOORD;
};

ConstantBuffer<FrameCBuffer> gFrameCBuffer : register(b0);

struct Output {
	float4 mPosH : SV_POSITION;
	float3 mViewRayV : VIEW_RAY;
};

Output main(in const Input input) {
	Output output;
	 
	output.mPosH = float4(input.mPosH, 1.0f);
	output.mViewRayV = mul(output.mPosH, gFrameCBuffer.mInvP).xyz;
	
	return output;
}