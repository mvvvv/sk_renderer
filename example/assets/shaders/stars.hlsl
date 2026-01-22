//--name = stars

#include "common.hlsli"

// Uncomment to disable pixel snapping (may cause sparkle)
//#define STARS_NO_PIXEL_SNAP

struct Inst {
	float4x4 world;
};
StructuredBuffer<Inst> inst : register(t2, space0);

struct vsIn {
	float3 pos   : SV_POSITION;
	float3 norm  : NORMAL;
	float2 uv    : TEXCOORD0;  // uv.x = vertex index (0, 1, 2), uv.y = brightness
	float4 color : COLOR0;     // brightness in RGB
};

struct psIn {
	float4 pos   : SV_POSITION;
	float4 color : COLOR0;
	uint   layer : SV_RenderTargetArrayIndex;
};

psIn vs(vsIn input, uint id : SV_InstanceID) {
	// Multi-view instancing: extract instance index and view index
	uint inst_idx = id / view_count;
	uint view_idx = id % view_count;

	psIn output;

	// Transform star center to world space, then to clip space
	float4 world_pos = mul(float4(input.pos, 1), inst[inst_idx].world);
	float4 clip_pos  = mul(world_pos, viewproj[view_idx]);

//#ifndef STARS_NO_PIXEL_SNAP
//	// Partially snap star center to pixel grid to reduce sparkle while keeping some smoothness
//	// Convert to NDC, blend toward pixel centers, convert back to clip space
//	float2 ndc = clip_pos.xy / clip_pos.w;
//	float2 pixel_coord = ndc * screen_size.xy * 0.5;            // NDC [-1,1] -> pixels
//	float2 snapped = floor(pixel_coord) + 0.5;                  // Pixel center
//	pixel_coord = lerp(pixel_coord, snapped, 0.5);              // Blend halfway
//	ndc = pixel_coord * screen_size.zw * 2.0;                   // Back to NDC
//	clip_pos.xy = ndc * clip_pos.w;
//#endif

	// Get vertex index (0, 1, or 2) to determine triangle expansion direction
	int vertex_idx = (int)input.uv.x;

	// Calculate pixel size in clip space
	// In NDC, 1 pixel = 2/width in X and 2/height in Y
	// To convert to clip space: multiply by w (perspective correction)
	float2 pixel_size = screen_size.zw * 2.0 * clip_pos.w;

	// Expand triangle to cover approximately 1 pixel
	// Use a small equilateral-ish triangle pattern
	// Make it slightly larger than 1 pixel to ensure it rasterizes
	float2 offsets[3] = {
		float2( 0.0,  1.0),   // top
		float2(-0.866, -0.5), // bottom-left
		float2( 0.866, -0.5)  // bottom-right
	};

	// Scale factor to make triangle ~1.5 pixels across (ensures rasterization)
	float scale = 1.0; //0.75;
	clip_pos.xy += offsets[vertex_idx] * pixel_size * scale;

	output.pos   = clip_pos;
	output.color = input.color;
	output.layer = view_idx;

	return output;
}

float4 ps(psIn input) : SV_TARGET {
	return input.color;
}
