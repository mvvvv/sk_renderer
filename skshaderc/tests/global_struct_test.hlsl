// Test shader for struct as free-floating value in $Globals buffer

struct circle_mask_t {
	float2 center;
	float  radius_inner;
	float  radius_outer;
};

// Free-floating struct in $Globals
circle_mask_t circle_mask;

// Some additional loose uniforms to test layout
float intensity;
float2 offset;

Texture2D    input_tex   : register(t0);
SamplerState input_tex_s : register(s0);

RWTexture2D<float4> output_tex : register(u0);

[numthreads(8, 8, 1)]
void cs(uint3 id : SV_DispatchThreadID) {
	uint2 dims;
	output_tex.GetDimensions(dims.x, dims.y);
	if (id.x >= dims.x || id.y >= dims.y) return;

	float2 uv = (float2(id.xy) + 0.5) / float2(dims);
	float4 color = input_tex.SampleLevel(input_tex_s, uv + offset, 0);

	// Apply circle mask using the struct
	float2 pos = uv - circle_mask.center;
	float dist = length(pos);
	float mask = smoothstep(circle_mask.radius_inner, circle_mask.radius_outer, dist);

	color.rgb *= lerp(1.0, mask, intensity);

	output_tex[id.xy] = color;
}
