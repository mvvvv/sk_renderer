// video_ahb.hlsl - AHB video display with hardware YCbCr conversion
// The VkSamplerYcbcrConversion baked into the immutable sampler handles
// YUV→RGB, so this shader just samples and converts sRGB→linear.
//--name = video_ahb

#include "common.hlsli"

// UV crop for codec padding (e.g., H.264 macroblocks round up to 16px)
float2 uv_crop;

// Single combined image sampler with YCbCr conversion (immutable sampler at t0/s0)
Texture2D    tex_video   : register(t0);
SamplerState tex_video_s : register(s0);

// Instance data
struct Inst {
	float4x4 world;
};
StructuredBuffer<Inst> inst : register(t2, space0);

struct vsIn {
	float3 pos   : SV_POSITION;
	float3 norm  : NORMAL;
	float2 uv    : TEXCOORD0;
	float4 color : COLOR0;
};

struct psIn {
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
};

psIn vs(vsIn input, uint id : SV_InstanceID) {
	psIn o;
	uint inst_idx = id / view_count;
	uint view_idx = id % view_count;

	float4 scaled_pos = mul(float4(input.pos, 1), inst[inst_idx].world);
	o.pos = scaled_pos;

	o.uv = input.uv * uv_crop;
	return o;
}

// sRGB to linear conversion (needed because render target is sRGB)
float3 srgb_to_linear(float3 srgb) {
	return pow(srgb, 2.2);
}

float4 ps(psIn input) : SV_TARGET {
	// Hardware YCbCr conversion produces sRGB RGB values
	float3 rgb = tex_video.Sample(tex_video_s, input.uv).rgb;

	// Convert to linear for sRGB render target
	float3 linear_rgb = srgb_to_linear(saturate(rgb));

	return float4(linear_rgb, 1.0);
}
