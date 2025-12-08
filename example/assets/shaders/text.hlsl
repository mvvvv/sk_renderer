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
	float3 pos;             // World position - 12 bytes, offset 0
	uint   glyph_index;     // Index into glyphs array - 4 bytes, offset 12
	float3 right;           // X axis * scale - 12 bytes, offset 16
	uint   color;           // Packed RGBA8 (0xAABBGGRR) - 4 bytes, offset 28
	float3 up;              // Y axis * scale - 12 bytes, offset 32
	uint   _pad;            // Padding - 4 bytes, offset 44
};                          // 48 bytes total

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

// Unpack RGBA8 color from uint (0xAABBGGRR format)
float3 unpack_color(uint packed) {
	float r = float((packed >>  0) & 0xFF) / 255.0;
	float g = float((packed >>  8) & 0xFF) / 255.0;
	float b = float((packed >> 16) & 0xFF) / 255.0;
	return float3(r, g, b);
}

psIn vs(vsIn input, uint id : SV_InstanceID) {
	// Multi-view instancing
	uint inst_idx = id / view_count;
	uint view_idx = id % view_count;

	Instance instance = inst[inst_idx];
	Glyph glyph = glyphs[instance.glyph_index];

	// Transform quad vertex to glyph bounds
	float2 glyph_size = glyph.bounds_max - glyph.bounds_min;
	float2 local_pos  = glyph.bounds_min + input.uv * glyph_size;

	// Transform to world space using position + right/up vectors
	// This is simpler and faster than full matrix multiply for 2D glyphs
	float3 world_pos = instance.pos
	                 + local_pos.x * instance.right
	                 + local_pos.y * instance.up;

	psIn output;
	output.pos       = mul(float4(world_pos, 1), viewproj[view_idx]);
	output.glyph_uv  = input.uv;
	output.glyph_pos = local_pos;
	output.glyph_idx = instance.glyph_index;
	output.color     = unpack_color(instance.color);
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

	// Quick rejection: curve AABB max_x < 0 means ray can't hit it
	// Based on IQ's quadratic Bezier AABB
	{
		float2 aa = p0 - 2.0 * p1 + p2;
		float  bx = p1.x - p0.x;
		float  tx = clamp(-bx / aa.x, 0.0, 1.0);
		float  qx = p0.x + tx * (2.0 * bx + tx * aa.x);
		if (max(max(p0.x, p2.x), qx) < 0.0) {
			return 0.0;
		}
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

// Analytical signed distance to quadratic Bezier curve
// Based on Inigo Quilez's closed-form solution
// https://iquilezles.org/articles/distfunctions2d/
float curve_distance(float2 pos, Curve c) {
	float2 A = c.p0;
	float2 B = c.p1;
	float2 C = c.p2;

	float2 a = B - A;
	float2 b = A - 2.0 * B + C;
	float2 c_coef = a * 2.0;
	float2 d = A - pos;

	// Handle near-linear curves (b ≈ 0) with line segment distance
	float bb = dot(b, b);
	if (bb < 1e-8) {
		float2 ac = C - A;
		float t = clamp(dot(pos - A, ac) / dot(ac, ac), 0.0, 1.0);
		return length(pos - (A + ac * t));
	}

	float kk = 1.0 / bb;
	float kx = kk * dot(a, b);
	float ky = kk * (2.0 * dot(a, a) + dot(d, b)) / 3.0;
	float kz = kk * dot(d, a);

	float p  = ky - kx * kx;
	float p3 = p * p * p;
	float q  = kx * (2.0 * kx * kx - 3.0 * ky) + kz;
	float h  = q * q + 4.0 * p3;

	float res;
	if (h >= 0.0) {
		// One real root
		h = sqrt(h);
		float2 x  = (float2(h, -h) - q) / 2.0;
		float2 uv = sign(x) * pow(abs(x), 1.0 / 3.0);
		float  t  = clamp(uv.x + uv.y - kx, 0.0, 1.0);
		float2 dd = d + (c_coef + b * t) * t;
		res = dot(dd, dd);
	} else {
		// Three real roots
		float z = sqrt(-p);
		float v = acos(q / (p * z * 2.0)) / 3.0;
		float m = cos(v);
		float n = sin(v) * 1.732050808;
		float3 t = clamp(float3(m + m, -n - m, n - m) * z - kx, 0.0, 1.0);

		float2 d1 = d + (c_coef + b * t.x) * t.x;
		float2 d2 = d + (c_coef + b * t.y) * t.y;
		res = min(dot(d1, d1), dot(d2, d2));
	}
	return sqrt(res);
}

///////////////////////////////////////////////////////////////////////////////
// Fragment Shader
///////////////////////////////////////////////////////////////////////////////

float4 ps(psIn input) : SV_TARGET {
	Glyph  glyph = glyphs[input.glyph_idx];
	float2 pos   = input.glyph_pos;

	// Determine which band this pixel falls into based on Y position
	float glyph_height = glyph.bounds_max.y - glyph.bounds_min.y;
	float normalized_y = (pos.y - glyph.bounds_min.y) / max(glyph_height, 1e-6);
	uint  band_idx     = clamp((uint)(normalized_y * BAND_COUNT), 0, BAND_COUNT - 1);

	// Get the band for this Y coordinate
	Band band = bands[glyph.band_start + band_idx];

	// First pass: compute winding number only (cheap)
	float winding = 0.0;
	for (uint i = 0; i < band.curve_count; i++) {
		Curve c = curves[band.curve_start + i];
		winding += curve_winding(pos, c);
	}

	// Determine if inside (non-zero winding rule)
	bool inside = abs(winding) > 0.5;

	// Interior pixels are fully opaque - skip expensive distance calculation
	if (inside) {
		return float4(input.color, 1.0);
	}

	// Outside pixels: compute distance for anti-aliased edges
	float min_dist = 1e10;
	for (uint j = 0; j < band.curve_count; j++) {
		Curve c = curves[band.curve_start + j];

		// Quick AABB rejection: skip curves whose bounding box is farther than current best
		// Uses IQ's quadratic Bezier AABB to get tight bounds
		float2 aa = c.p0 - 2.0 * c.p1 + c.p2;
		float2 bb = c.p1 - c.p0;
		float2 tt = clamp(-bb / aa, 0.0, 1.0);
		float2 qq = c.p0 + tt * (2.0 * bb + tt * aa);
		float2 box_min = min(min(c.p0, c.p2), qq);
		float2 box_max = max(max(c.p0, c.p2), qq);

		// Distance from pos to AABB (0 if inside)
		float2 d_box = max(box_min - pos, pos - box_max);
		float  aabb_dist_sq = dot(max(d_box, 0.0), max(d_box, 0.0));
		if (aabb_dist_sq > min_dist * min_dist) continue;

		min_dist = min(min_dist, curve_distance(pos, c));
	}

	// Anti-aliasing based on screen-space derivatives
	float2 dpos_dx    = ddx(pos);
	float2 dpos_dy    = ddy(pos);
	float  pixel_size = length(float2(length(dpos_dx), length(dpos_dy)));

	float coverage = saturate(1.0 - min_dist / pixel_size);

	// Early discard for fully transparent pixels
	if (coverage < 0.01) {
		discard;
	}

	return float4(input.color, coverage);
}
