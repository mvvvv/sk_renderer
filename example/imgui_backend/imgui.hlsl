// SPDX-License-Identifier: MIT
// Dear ImGui shader for sk_renderer backend
// Renders textured triangles with vertex colors

float4x4 projection_matrix;

struct VS_INPUT {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

Texture2D    texture0 : register(t0);
SamplerState sampler0 : register(s0);

// Convert sRGB to linear for correct color blending
float3 srgb_to_linear(float3 srgb) {
    return pow(srgb, 2.2);
}

PS_INPUT vs(VS_INPUT input) {
    PS_INPUT output;
    output.pos = mul(projection_matrix, float4(input.pos.xy, 0.0, 1.0));
    // Convert vertex color from sRGB to linear
    output.col = float4(srgb_to_linear(input.col.rgb), input.col.a);
    output.uv  = input.uv;
    return output;
}

float4 ps(PS_INPUT input) : SV_Target {
    float4 tex_color = texture0.Sample(sampler0, input.uv);
    return input.col * tex_color;
}
