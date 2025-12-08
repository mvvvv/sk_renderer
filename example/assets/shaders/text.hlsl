//--name = text_vector

// GPU-evaluated vector text rendering
// Evaluates quadratic Bezier curves directly in the fragment shader
// for resolution-independent text at any scale or angle.

#include "common.hlsli"

///////////////////////////////////////////////////////////////////////////////
// GPU Buffer Structures (must match C exactly)
///////////////////////////////////////////////////////////////////////////////

struct Curve {
	float2 p0;      // Start point
	float2 p1;      // Control point
	float2 p2;      // End point
	float  y_min;   // Curve Y bounds
	float  y_max;
};

struct Band {
	uint curve_start;   // Index into curves array
	uint curve_count;   // Number of curves in this band
};

#define BAND_COUNT 16   // Must match TEXT_BAND_COUNT in C

struct Glyph {
	uint   band_start;      // Index into bands array (BAND_COUNT bands per glyph)
	uint   curve_start;     // Index into curves array (for fallback)
	uint   curve_count;     // Total number of curves for this glyph
	uint   _pad0;           // Padding for alignment
	float2 bounds_min;      // Glyph bounding box
	float2 bounds_max;
	float  advance;         // Horizontal advance
	float  lsb;             // Left side bearing
};

struct Instance {
	float4x4 transform;     // World transform - 64 bytes, offset 0
	uint     glyph_index;   // Index into glyphs array - 4 bytes, offset 64
	uint     _pad0;         // Padding - 4 bytes, offset 68
	uint     _pad1;         // Padding - 4 bytes, offset 72
	uint     _pad2;         // Padding - 4 bytes, offset 76
	float4   color;         // RGBA color - 16 bytes, offset 80 (16-byte aligned)
};                          // 96 bytes total

///////////////////////////////////////////////////////////////////////////////
// Buffers
///////////////////////////////////////////////////////////////////////////////

// Instance data - passed via skr_render_list_add (automatic instancing)
StructuredBuffer<Instance> inst : register(t2, space0);

// Font data - bound via skr_material_set_buffer
StructuredBuffer<Curve>    curves : register(t3);
StructuredBuffer<Band>     bands  : register(t4);
StructuredBuffer<Glyph>    glyphs : register(t5);

///////////////////////////////////////////////////////////////////////////////
// Vertex Shader
///////////////////////////////////////////////////////////////////////////////

struct vsIn {
	float2 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
};

struct psIn {
	float4 pos       : SV_POSITION;
	float2 glyph_uv  : TEXCOORD0;   // UV in glyph bounds [0,1]
	float2 glyph_pos : TEXCOORD1;   // Position in glyph space
	nointerpolation uint glyph_idx : TEXCOORD2;
	float3 color     : COLOR0;
	uint   layer     : SV_RenderTargetArrayIndex;  // Multi-view output layer
};

psIn vs(vsIn input, uint id : SV_InstanceID) {
	// Multi-view instancing
	uint inst_idx = id / view_count;
	uint view_idx = id % view_count;

	Instance instance = inst[inst_idx];
	Glyph glyph = glyphs[instance.glyph_index];

	// Transform quad vertex to glyph bounds
	float2 glyph_size = glyph.bounds_max - glyph.bounds_min;
	float2 local_pos = glyph.bounds_min + input.uv * glyph_size;

	// Transform to world space
	float4 world_pos = mul(float4(local_pos, 0, 1), instance.transform);

	psIn output;
	output.pos       = mul(world_pos, viewproj[view_idx]);
	output.glyph_uv  = input.uv;
	output.glyph_pos = local_pos;
	output.glyph_idx = instance.glyph_index;
	output.color     = instance.color.rgb;
	output.layer     = view_idx;  // Route to correct render target layer

	return output;
}

///////////////////////////////////////////////////////////////////////////////
// Curve Evaluation Functions
///////////////////////////////////////////////////////////////////////////////

// Evaluate quadratic Bezier at parameter t
float2 bezier_eval(float2 p0, float2 p1, float2 p2, float t) {
	float it = 1.0 - t;
	return it * it * p0 + 2.0 * it * t * p1 + t * t * p2;
}

// Compute the Y derivative of quadratic Bezier at parameter t
// d/dt B(t) = 2(1-t)(p1-p0) + 2t(p2-p1) => y component
float bezier_dy(float y0, float y1, float y2, float t) {
	return 2.0 * (1.0 - t) * (y1 - y0) + 2.0 * t * (y2 - y1);
}

// Calculate winding contribution from a single quadratic Bezier curve.
// Uses the standard approach: count ray crossings in +X direction.
// Returns +1 or -1 based on crossing direction, 0 if no crossing.
float curve_winding(float2 pos, Curve c) {
	// Translate curve so pos is at origin
	float2 p0 = c.p0 - pos;
	float2 p1 = c.p1 - pos;
	float2 p2 = c.p2 - pos;

	// Quick rejection: if all Y coordinates are same sign, no crossing
	if ((p0.y > 0 && p1.y > 0 && p2.y > 0) ||
	    (p0.y < 0 && p1.y < 0 && p2.y < 0)) {
		return 0.0;
	}

	// Solve: B_y(t) = 0 where B_y(t) = (1-t)^2*p0.y + 2(1-t)t*p1.y + t^2*p2.y
	// Expanding: a*t^2 + b*t + c = 0
	float a = p0.y - 2.0 * p1.y + p2.y;
	float b = 2.0 * (p1.y - p0.y);
	float c_coef = p0.y;

	float winding = 0.0;

	if (abs(a) < 1e-6) {
		// Linear case (degenerate quadratic)
		if (abs(b) > 1e-6) {
			float t = -c_coef / b;
			if (t >= 0.0 && t <= 1.0) {
				// Check if crossing is to the right (positive X)
				float2 pt = bezier_eval(p0, p1, p2, t);
				if (pt.x > 0.0) {
					// Direction based on Y derivative sign
					float dy = bezier_dy(p0.y, p1.y, p2.y, t);
					winding = (dy > 0.0) ? 1.0 : -1.0;
				}
			}
		}
	} else {
		// Quadratic case
		float discriminant = b * b - 4.0 * a * c_coef;
		if (discriminant >= 0.0) {
			float sqrt_d = sqrt(discriminant);
			float inv_2a = 0.5 / a;

			// Two potential roots
			float t1 = (-b - sqrt_d) * inv_2a;
			float t2 = (-b + sqrt_d) * inv_2a;

			// Check first root
			if (t1 >= 0.0 && t1 <= 1.0) {
				float2 pt = bezier_eval(p0, p1, p2, t1);
				if (pt.x > 0.0) {
					float dy = bezier_dy(p0.y, p1.y, p2.y, t1);
					winding += (dy > 0.0) ? 1.0 : -1.0;
				}
			}

			// Check second root (only if different from first)
			if (t2 >= 0.0 && t2 <= 1.0 && abs(t2 - t1) > 1e-6) {
				float2 pt = bezier_eval(p0, p1, p2, t2);
				if (pt.x > 0.0) {
					float dy = bezier_dy(p0.y, p1.y, p2.y, t2);
					winding += (dy > 0.0) ? 1.0 : -1.0;
				}
			}
		}
	}

	return winding;
}

// Compute signed distance to quadratic Bezier curve (approximate)
// Used for anti-aliasing
float curve_distance(float2 pos, Curve c) {
	// Use Newton's method to find closest point on curve
	// Start with t = 0.5 and iterate
	float2 p0 = c.p0;
	float2 p1 = c.p1;
	float2 p2 = c.p2;

	float t = 0.5;
	float min_dist_sq = 1e10;

	// Sample a few points and pick closest
	for (int i = 0; i <= 4; i++) {
		float s = i / 4.0;
		float2 pt = bezier_eval(p0, p1, p2, s);
		float2 d = pt - pos;
		float dist_sq = dot(d, d);
		if (dist_sq < min_dist_sq) {
			min_dist_sq = dist_sq;
			t = s;
		}
	}

	// Refine with a few Newton iterations
	for (int iter = 0; iter < 3; iter++) {
		float2 pt = bezier_eval(p0, p1, p2, t);
		float2 d = pt - pos;

		// Derivative of distance^2 w.r.t. t
		float2 tangent = 2.0 * (1.0 - t) * (p1 - p0) + 2.0 * t * (p2 - p1);
		float ddist = 2.0 * dot(d, tangent);

		// Second derivative (approximate)
		float2 accel = 2.0 * (p2 - 2.0 * p1 + p0);
		float d2dist = 2.0 * (dot(tangent, tangent) + dot(d, accel));

		if (abs(d2dist) > 1e-6) {
			t = t - ddist / d2dist;
			t = clamp(t, 0.0, 1.0);
		}
	}

	float2 closest = bezier_eval(p0, p1, p2, t);
	return length(closest - pos);
}

///////////////////////////////////////////////////////////////////////////////
// Fragment Shader
///////////////////////////////////////////////////////////////////////////////

float4 ps(psIn input) : SV_TARGET {
	Glyph glyph = glyphs[input.glyph_idx];
	float2 pos = input.glyph_pos;

	// Determine which band this pixel falls into based on Y position
	float glyph_height = glyph.bounds_max.y - glyph.bounds_min.y;
	float normalized_y = (pos.y - glyph.bounds_min.y) / max(glyph_height, 1e-6);
	uint band_idx = clamp((uint)(normalized_y * BAND_COUNT), 0, BAND_COUNT - 1);

	// Get the band for this Y coordinate
	Band band = bands[glyph.band_start + band_idx];

	// Sum winding contributions from curves in this band only
	float winding = 0.0;
	float min_dist = 1e10;

	for (uint i = 0; i < band.curve_count; i++) {
		Curve c = curves[band.curve_start + i];
		winding += curve_winding(pos, c);

		// Also compute distance for anti-aliasing
		float dist = curve_distance(pos, c);
		min_dist = min(min_dist, dist);
	}

	// Determine if inside (non-zero winding rule)
	bool inside = abs(winding) > 0.5;

	// Anti-aliasing based on screen-space derivatives
	float2 dpos_dx = ddx(pos);
	float2 dpos_dy = ddy(pos);
	float pixel_size = length(float2(length(dpos_dx), length(dpos_dy)));

	// Coverage calculation:
	// - Inside: solid (no fading from internal curves)
	// - Outside: soft edge based on distance
	float coverage;
	if (inside) {
		coverage = 1.0;
	} else {
		// Soft outer edge for anti-aliasing
		coverage = saturate(1.0 - min_dist / pixel_size);
	}

	// Early discard for fully transparent pixels
	if (coverage < 0.01) {
		discard;
	}

	return float4(input.color, coverage);
}
