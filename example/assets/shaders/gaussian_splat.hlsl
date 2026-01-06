//--name = gaussian_splat

#include "common.hlsli"

// Render parameters
float  splat_scale;      // Global scale multiplier for splats
float  sh_degree;        // SH degree to use (0-3)
uint   splat_count;      // Total number of splats
float  opacity_scale;    // Opacity multiplier
float2 screen_size;      // Screen resolution in pixels
float  max_radius;       // Max splat radius in pixels (0 = unlimited)

// Gaussian splat data - packed for efficiency (with HLSL alignment padding)
// Position (12) + Opacity (4) = 16 bytes
// SH DC (12) + pad (4) = 16 bytes
// Scale (12) + pad (4) = 16 bytes
// Rotation quat = 16 bytes
// SH rest (15 x float4) = 240 bytes
// Total = 304 bytes per splat

struct GaussianSplat {
	float3 position;
	float  opacity;
	float3 sh_dc;      // f_dc_0, f_dc_1, f_dc_2
	float  _pad1;      // Padding to match C struct alignment
	float3 scale;      // scale_0, scale_1, scale_2 (log scale)
	float  _pad2;      // Padding to match C struct alignment
	float4 rotation;   // rot_0, rot_1, rot_2, rot_3 (quaternion)
	float4 sh_rest[15]; // f_rest in groups of 3 (HLSL pads float3[] to float4[])
};

StructuredBuffer<GaussianSplat> splats       : register(t3, space0);
StructuredBuffer<uint>          sort_indices : register(t4, space0);

struct vsIn {
	float3 pos   : SV_POSITION;  // Quad vertex position (-1 to 1)
	float3 norm  : NORMAL;
	float2 uv    : TEXCOORD0;
	float4 color : COLOR0;
};

struct psIn {
	float4 pos        : SV_POSITION;
	float2 uv         : TEXCOORD0;     // Pixel offset from center
	float3 color      : COLOR0;
	float  opacity    : COLOR1;
	float2 conic_xy   : TEXCOORD1;
	float  conic_z    : TEXCOORD2;
	float2 major_axis : TEXCOORD3;     // Major axis direction (normalized)
	float2 axis_lens  : TEXCOORD4;     // (major_len, minor_len) for edge fade
};

// Spherical harmonics constants
static const float SH_C0 = 0.28209479177387814f;
static const float SH_C1 = 0.4886025119029199f;
static const float SH_C2_0 = 1.0925484305920792f;
static const float SH_C2_1 = -1.0925484305920792f;
static const float SH_C2_2 = 0.31539156525252005f;
static const float SH_C2_3 = -1.0925484305920792f;
static const float SH_C2_4 = 0.5462742152960396f;
static const float SH_C3_0 = -0.5900435899266435f;
static const float SH_C3_1 = 2.890611442640554f;
static const float SH_C3_2 = -0.4570457994644658f;
static const float SH_C3_3 = 0.3731763325901154f;
static const float SH_C3_4 = -0.4570457994644658f;
static const float SH_C3_5 = 1.445305721320277f;
static const float SH_C3_6 = -0.5900435899266435f;

// Evaluate spherical harmonics for a given direction
float3 eval_sh(GaussianSplat splat, float3 dir) {
	// Start with DC term (order 0)
	float3 result = SH_C0 * splat.sh_dc;

	if (sh_degree < 1) return result;

	// Order 1
	float x = dir.x;
	float y = dir.y;
	float z = dir.z;

	result += SH_C1 * (-y * splat.sh_rest[0].xyz + z * splat.sh_rest[1].xyz - x * splat.sh_rest[2].xyz);

	if (sh_degree < 2) return result;

	// Order 2
	float xx = x * x, yy = y * y, zz = z * z;
	float xy = x * y, yz = y * z, xz = x * z;

	result += SH_C2_0 * xy * splat.sh_rest[3].xyz;
	result += SH_C2_1 * yz * splat.sh_rest[4].xyz;
	result += SH_C2_2 * (2.0f * zz - xx - yy) * splat.sh_rest[5].xyz;
	result += SH_C2_3 * xz * splat.sh_rest[6].xyz;
	result += SH_C2_4 * (xx - yy) * splat.sh_rest[7].xyz;

	if (sh_degree < 3) return result;

	// Order 3
	result += SH_C3_0 * y * (3.0f * xx - yy) * splat.sh_rest[8].xyz;
	result += SH_C3_1 * xy * z * splat.sh_rest[9].xyz;
	result += SH_C3_2 * y * (4.0f * zz - xx - yy) * splat.sh_rest[10].xyz;
	result += SH_C3_3 * z * (2.0f * zz - 3.0f * xx - 3.0f * yy) * splat.sh_rest[11].xyz;
	result += SH_C3_4 * x * (4.0f * zz - xx - yy) * splat.sh_rest[12].xyz;
	result += SH_C3_5 * z * (xx - yy) * splat.sh_rest[13].xyz;
	result += SH_C3_6 * x * (xx - 3.0f * yy) * splat.sh_rest[14].xyz;

	return result;
}

// Build rotation matrix from quaternion
// Input quaternion is in (w, x, y, z) order from PLY (rot_0, rot_1, rot_2, rot_3)
float3x3 quat_to_matrix(float4 q) {
	float w = q.x;
	float x = q.y;
	float y = q.z;
	float z = q.w;

	return float3x3(
		1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - w * z), 2.0f * (x * z + w * y),
		2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - w * x),
		2.0f * (x * z - w * y), 2.0f * (y * z + w * x), 1.0f - 2.0f * (x * x + y * y)
	);
}

// Compute 2D covariance matrix from 3D Gaussian (in pixel space, following original 3DGS)
void compute_cov2d(float3 mean, float3 scale, float4 rotation, uint view_idx,
                   out float3 cov2d, out float2 screen_pos) {
	// Build 3D covariance matrix: Sigma = M * M^T where M = R * S
	float3x3 R = quat_to_matrix(rotation);

	// Scale matrix (diagonal) - convert from log scale
	float3 s = exp(scale) * splat_scale;
	float3x3 S = float3x3(
		s.x, 0, 0,
		0, s.y, 0,
		0, 0, s.z
	);

	float3x3 M = mul(R, S);
	float3x3 Sigma = mul(M, transpose(M));

	// Transform to view space
	float4x4 V = view[view_idx];
	float4 p_view = mul(float4(mean, 1), V);

	// Check if behind camera (right-handed: objects in front have negative Z)
	float view_z = -p_view.z;  // Flip to positive-forward convention
	if (view_z <= 0.1f) {
		cov2d = float3(0, 0, 0);
		screen_pos = float2(-1000, -1000);
		return;
	}

	// Convert NDC focal length to pixel focal length (original 3DGS uses pixels)
	// NDC focal = 1/tan(fov/2), pixel focal = (screen_size/2) / tan(fov/2)
	float focal_ndc_x = projection[view_idx][0][0];
	float focal_ndc_y = projection[view_idx][1][1];
	float focal_x = focal_ndc_x * screen_size.x * 0.5f;
	float focal_y = focal_ndc_y * screen_size.y * 0.5f;

	// Clamp view-space position to avoid extreme distortion at edges
	float tan_fov_x = 1.0f / focal_ndc_x;
	float tan_fov_y = 1.0f / focal_ndc_y;
	float limx = 1.3f * tan_fov_x;
	float limy = 1.3f * tan_fov_y;

	float txz = clamp(p_view.x / view_z, -limx, limx);
	float tyz = clamp(p_view.y / view_z, -limy, limy);

	float x = txz * view_z;
	float y = tyz * view_z;

	// Jacobian of perspective projection (now in pixel space)
	// Third column: d(screen)/d(p_view.z) = d(screen)/d(view_z) * d(view_z)/d(p_view.z)
	// Since view_z = -p_view.z, we get an extra -1 factor, making the result positive
	float3x3 J = float3x3(
		focal_x / view_z, 0, focal_x * x / (view_z * view_z),
		0, focal_y / view_z, focal_y * y / (view_z * view_z),
		0, 0, 0
	);

	// Transform covariance to screen space (pixel²)
	// T = J * W, cov2D = T * Σ * T^T
	// W is the rotation part of the view matrix (world -> camera rotation)
	float3x3 W = transpose((float3x3)V);
	float3x3 T = mul(J, W);
	float3x3 cov = mul(T, mul(Sigma, transpose(T)));

	// Extract 2x2 covariance (ignore z)
	// Low-pass filter of 0.3 ensures minimum ~1 pixel splat size (standard 3DGS value)
	cov2d = float3(cov[0][0] + 0.3f, cov[0][1], cov[1][1] + 0.3f);

	// Project to screen (NDC)
	float4 p_clip = mul(float4(mean, 1), viewproj[view_idx]);
	screen_pos = p_clip.xy / p_clip.w;
}

psIn vs(vsIn input, uint id : SV_InstanceID) {
	psIn output = (psIn)0;

	// Multi-view instancing
	uint splat_idx = id / view_count;
	uint view_idx  = id % view_count;

	if (splat_idx >= splat_count) {
		output.pos = float4(-1000, -1000, 0, 1);
		return output;
	}

	// Get sorted splat index
	uint sorted_idx = sort_indices[splat_idx];
	GaussianSplat splat = splats[sorted_idx];

	// Compute 2D covariance and screen position
	float3 cov2d;
	float2 screen_center;
	compute_cov2d(splat.position, splat.scale, splat.rotation, view_idx, cov2d, screen_center);

	// Invert 2x2 covariance to get conic
	float det = cov2d.x * cov2d.z - cov2d.y * cov2d.y;
	if (det <= 0.0f) {
		output.pos = float4(-1000, -1000, 0, 1);
		return output;
	}
	float det_inv = 1.0f / det;
	float3 conic = float3(cov2d.z * det_inv, -cov2d.y * det_inv, cov2d.x * det_inv);

	// Compute eigenvalues of 2D covariance (ellipse axis lengths squared)
	// cov2d is [xx, xy, yy] in pixel² space
	float mid = 0.5f * (cov2d.x + cov2d.z);
	float disc = max(0.0f, mid * mid - det);  // Allow near-zero for circular projections
	float lambda1 = mid + sqrt(disc);
	float lambda2 = max(0.3f, mid - sqrt(disc));  // Clamp result, not input (0.3 matches low-pass filter)

	// Axis lengths in pixels (3 sigma covers 99.7%)
	float major_len = 3.0f * sqrt(lambda1);
	float minor_len = 3.0f * sqrt(lambda2);

	// Compute eigenvector for lambda1 (major axis direction)
	// For symmetric matrix [a b; b c], eigenvector for λ is (b, λ-a) normalized
	float2 major_axis;
	if (abs(cov2d.y) > 1e-6f) {
		major_axis = normalize(float2(cov2d.y, lambda1 - cov2d.x));
	} else {
		// Diagonal matrix - axes are screen-aligned
		major_axis = (cov2d.x >= cov2d.z) ? float2(1, 0) : float2(0, 1);
	}
	float2 minor_axis = float2(-major_axis.y, major_axis.x);  // Perpendicular

	// Cap axis lengths to prevent massive overdraw
	// When capping, scale the conic so Gaussian still falls off properly at quad edge
	float orig_major_len = major_len;
	float orig_minor_len = minor_len;
	if (max_radius > 0.0f) {
		major_len = min(major_len, max_radius);
		minor_len = min(minor_len, max_radius);
	}
	// Scale conic by inverse square of cap ratio to preserve falloff at edge
	float scale_major = orig_major_len / major_len;  // >= 1 when capped
	float scale_minor = orig_minor_len / minor_len;
	// Conic transforms: scale along major axis by 1/s_major², along minor by 1/s_minor²
	// In the eigenvector basis: conic' = [[1/s_major², 0], [0, 1/s_minor²]] * conic
	// Transform to screen space using major/minor axis rotation
	float2x2 R = float2x2(major_axis.x, minor_axis.x,
	                      major_axis.y, minor_axis.y);
	float2x2 S = float2x2(scale_major * scale_major, 0,
	                      0, scale_minor * scale_minor);
	float2x2 conic_mat = float2x2(conic.x, conic.y, conic.y, conic.z);
	float2x2 conic_scaled = mul(mul(R, mul(S, transpose(R))), conic_mat);
	conic = float3(conic_scaled[0][0], conic_scaled[0][1], conic_scaled[1][1]);

	// Transform quad vertex by ellipse axes (oriented rectangle)
	// input.pos.xy is in [-1, 1], so this gives us a tight-fitting quad
	float2 quad_offset_pixels = input.pos.x * major_axis * major_len +
	                            input.pos.y * minor_axis * minor_len;

	// Convert to NDC
	float2 quad_offset_ndc = quad_offset_pixels / (screen_size * 0.5f);

	// Clamp to reasonable bounds
	quad_offset_ndc = clamp(quad_offset_ndc, -1.0f, 1.0f);

	// Position the quad centered at screen_center
	float2 ndc_pos = screen_center + quad_offset_ndc;

	// Get depth for proper ordering
	float4 p_clip = mul(float4(splat.position, 1), viewproj[view_idx]);
	float depth = p_clip.z / p_clip.w;

	output.pos = float4(ndc_pos, depth, 1);
	// UV = actual pixel offset from center for Gaussian falloff
	output.uv = quad_offset_pixels;

	// Compute color from spherical harmonics
	// Direction should be FROM splat TO camera (view direction), matching Aras's implementation
	float3 view_dir = normalize(cam_pos[view_idx].xyz - splat.position);
	float3 sh_color = eval_sh(splat, view_dir);
	output.color = max(sh_color + 0.5f, 0.0f); // SH outputs [-0.5, inf), shift to [0, inf)

	// Sigmoid activation for opacity
	output.opacity = 1.0f / (1.0f + exp(-splat.opacity)) * opacity_scale;

	output.conic_xy = conic.xy;
	output.conic_z = conic.z;
	output.major_axis = major_axis;
	output.axis_lens = float2(major_len, minor_len);

	return output;
}

float4 ps(psIn input) : SV_TARGET {
	// Compute Gaussian falloff
	float2 d = input.uv;
	float power = -0.5f * (input.conic_xy.x * d.x * d.x +
	                        input.conic_z * d.y * d.y +
	                        2.0f * input.conic_xy.y * d.x * d.y);

	if (power > 0.0f) discard;

	float alpha = min(0.99f, input.opacity * exp(power));
	if (alpha < 1.0f / 255.0f) discard;

	float3 color_srgb = pow(input.color, 2.2f);
	return float4(color_srgb * alpha, alpha);
}
