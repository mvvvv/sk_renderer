// float_math_amalgamated.h - Single-file distribution
// Generated automatically - do not edit
//
// This is an amalgamated version of float_math containing all implementations
// in a single header file for easy distribution.
//
// Usage: Just include this file. Platform detection is automatic.
//
// To force a specific implementation, define before including:
//   #define FLOAT_MATH_FORCE_FALLBACK 1  // Force scalar implementation
//   #define FLOAT_MATH_FORCE_SSE 1       // Force SSE implementation
//   #define FLOAT_MATH_FORCE_NEON 1      // Force NEON implementation

#ifndef FLOAT_MATH_AMALGAMATED_H
#define FLOAT_MATH_AMALGAMATED_H

// Platform detection and implementation selection
#if defined(FLOAT_MATH_FORCE_FALLBACK)
	#define FLOAT_MATH_USE_FALLBACK 1
#elif defined(FLOAT_MATH_FORCE_SSE)
	#define FLOAT_MATH_USE_SSE 1
#elif defined(FLOAT_MATH_FORCE_NEON)
	#define FLOAT_MATH_USE_NEON 1
#elif defined(__SSE4_1__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
	#define FLOAT_MATH_USE_SSE 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
	#define FLOAT_MATH_USE_NEON 1
#else
	#define FLOAT_MATH_USE_FALLBACK 1
#endif

// ============================================================================
// Fallback (scalar) implementation
// ============================================================================
#if defined(FLOAT_MATH_USE_FALLBACK)


#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Vector types (scalar fallback - no SIMD)
// ============================================================================

typedef struct { float x, y; } float2;
typedef struct { float x, y, z; } float3;
typedef struct { float x, y, z; } float3s; // Same as float3 in fallback (no SIMD padding)
typedef struct { float x, y, z, w; } float4;

// Matrix type (row-major for direct shader compatibility)
typedef struct {
	float m[16]; // Row-major: [m00, m01, m02, m03, m10, m11, ...]
} float4x4;

// Conversions between float3 and float3s (no-op in fallback)
static inline float3  float3s_to_float3(float3s v) { return (float3){v.x, v.y, v.z}; }
static inline float3s float3_to_float3s(float3 v)  { return (float3s){v.x, v.y, v.z}; }

// ============================================================================
// Float2 operations
// ============================================================================

static inline float2 float2_add  (float2 a, float2 b) { return (float2){a.x + b.x, a.y + b.y}; }
static inline float2 float2_add_s(float2 a, float  s) { return (float2){a.x + s, a.y + s}; }
static inline float2 float2_sub  (float2 a, float2 b) { return (float2){a.x - b.x, a.y - b.y}; }
static inline float2 float2_sub_s(float2 a, float  s) { return (float2){a.x - s, a.y - s}; }
static inline float2 float2_mul  (float2 a, float2 b) { return (float2){a.x * b.x, a.y * b.y}; }
static inline float2 float2_mul_s(float2 a, float  s) { return (float2){a.x * s, a.y * s}; }
static inline float2 float2_div  (float2 a, float2 b) { return (float2){a.x / b.x, a.y / b.y}; }
static inline float2 float2_div_s(float2 a, float  s) { return (float2){a.x / s, a.y / s}; }
static inline float  float2_dot  (float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
static inline float  float2_mag2 (float2 v)           { return float2_dot(v, v); }
static inline float  float2_dist2(float2 a, float2 b) { return float2_mag2(float2_sub(a, b)); }

static inline float2 float2_frac(float2 v) {
	return (float2){v.x - floorf(v.x), v.y - floorf(v.y)};
}

static inline float2 float2_floor(float2 v) {
	return (float2){floorf(v.x), floorf(v.y)};
}

static inline float2 float2_ceil(float2 v) {
	return (float2){ceilf(v.x), ceilf(v.y)};
}

static inline float2 float2_abs(float2 v) {
	return (float2){fabsf(v.x), fabsf(v.y)};
}

static inline float2 float2_min(float2 a, float2 b) {
	return (float2){fminf(a.x, b.x), fminf(a.y, b.y)};
}

static inline float2 float2_max(float2 a, float2 b) {
	return (float2){fmaxf(a.x, b.x), fmaxf(a.y, b.y)};
}

static inline float float2_mag(float2 v) {
	return sqrtf(float2_mag2(v));
}

static inline float float2_dist(float2 a, float2 b) {
	return sqrtf(float2_dist2(a, b));
}

static inline float2 float2_norm(float2 v) {
	float mag = float2_mag(v);
	if (mag == 0.0f) {
		return (float2){0.0f, 0.0f};
	}
	return float2_div_s(v, mag);
}

// ============================================================================
// Float3 operations
// ============================================================================

static inline float3 float3_add  (float3 a, float3 b) { return (float3){a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline float3 float3_add_s(float3 a, float  s) { return (float3){a.x + s, a.y + s, a.z + s}; }
static inline float3 float3_sub  (float3 a, float3 b) { return (float3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline float3 float3_sub_s(float3 a, float  s) { return (float3){a.x - s, a.y - s, a.z - s}; }
static inline float3 float3_mul  (float3 a, float3 b) { return (float3){a.x * b.x, a.y * b.y, a.z * b.z}; }
static inline float3 float3_mul_s(float3 a, float  s) { return (float3){a.x * s, a.y * s, a.z * s}; }
static inline float3 float3_div  (float3 a, float3 b) { return (float3){a.x / b.x, a.y / b.y, a.z / b.z}; }
static inline float3 float3_div_s(float3 a, float  s) { return (float3){a.x / s, a.y / s, a.z / s}; }
static inline float  float3_dot  (float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float  float3_mag2 (float3 v)           { return float3_dot(v, v); }
static inline float  float3_dist2(float3 a, float3 b) { return float3_mag2(float3_sub(a, b)); }
static inline float3 float3_cross(float3 a, float3 b) {
	return (float3){
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
}

static inline float3 float3_frac(float3 v) {
	return (float3){v.x - floorf(v.x), v.y - floorf(v.y), v.z - floorf(v.z)};
}

static inline float3 float3_floor(float3 v) {
	return (float3){floorf(v.x), floorf(v.y), floorf(v.z)};
}

static inline float3 float3_ceil(float3 v) {
	return (float3){ceilf(v.x), ceilf(v.y), ceilf(v.z)};
}

static inline float3 float3_abs(float3 v) {
	return (float3){fabsf(v.x), fabsf(v.y), fabsf(v.z)};
}

static inline float3 float3_min(float3 a, float3 b) {
	return (float3){fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z)};
}

static inline float3 float3_max(float3 a, float3 b) {
	return (float3){fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z)};
}

static inline float float3_mag(float3 v) {
	return sqrtf(float3_mag2(v));
}

static inline float float3_dist(float3 a, float3 b) {
	return sqrtf(float3_dist2(a, b));
}

static inline float3 float3_norm(float3 v) {
	float mag = float3_mag(v);
	if (mag == 0.0f) {
		return (float3){0.0f, 0.0f, 0.0f};
	}
	return float3_div_s(v, mag);
}

// ============================================================================
// Float3s operations (same as float3 in fallback - no SIMD benefit)
// ============================================================================

static inline float3s float3s_add  (float3s a, float3s b) { return (float3s){a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline float3s float3s_add_s(float3s a, float  s)  { return (float3s){a.x + s, a.y + s, a.z + s}; }
static inline float3s float3s_sub  (float3s a, float3s b) { return (float3s){a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline float3s float3s_sub_s(float3s a, float  s)  { return (float3s){a.x - s, a.y - s, a.z - s}; }
static inline float3s float3s_mul  (float3s a, float3s b) { return (float3s){a.x * b.x, a.y * b.y, a.z * b.z}; }
static inline float3s float3s_mul_s(float3s a, float  s)  { return (float3s){a.x * s, a.y * s, a.z * s}; }
static inline float3s float3s_div  (float3s a, float3s b) { return (float3s){a.x / b.x, a.y / b.y, a.z / b.z}; }
static inline float3s float3s_div_s(float3s a, float  s)  { return (float3s){a.x / s, a.y / s, a.z / s}; }
static inline float   float3s_dot  (float3s a, float3s b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float   float3s_mag2 (float3s v)            { return float3s_dot(v, v); }
static inline float   float3s_dist2(float3s a, float3s b) { return float3s_mag2(float3s_sub(a, b)); }
static inline float3s float3s_cross(float3s a, float3s b) {
	return (float3s){
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
}

static inline float3s float3s_frac(float3s v) {
	return (float3s){v.x - floorf(v.x), v.y - floorf(v.y), v.z - floorf(v.z)};
}

static inline float3s float3s_floor(float3s v) {
	return (float3s){floorf(v.x), floorf(v.y), floorf(v.z)};
}

static inline float3s float3s_ceil(float3s v) {
	return (float3s){ceilf(v.x), ceilf(v.y), ceilf(v.z)};
}

static inline float3s float3s_abs(float3s v) {
	return (float3s){fabsf(v.x), fabsf(v.y), fabsf(v.z)};
}

static inline float3s float3s_min(float3s a, float3s b) {
	return (float3s){fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z)};
}

static inline float3s float3s_max(float3s a, float3s b) {
	return (float3s){fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z)};
}

static inline float float3s_mag(float3s v) {
	return sqrtf(float3s_mag2(v));
}

static inline float float3s_dist(float3s a, float3s b) {
	return sqrtf(float3s_dist2(a, b));
}

static inline float3s float3s_norm(float3s v) {
	float mag = float3s_mag(v);
	if (mag == 0.0f) {
		return (float3s){0.0f, 0.0f, 0.0f};
	}
	return float3s_div_s(v, mag);
}

// ============================================================================
// Float4 operations
// ============================================================================

static inline float4 float4_add  (float4 a, float4 b) { return (float4){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
static inline float4 float4_add_s(float4 a, float  s) { return (float4){a.x + s, a.y + s, a.z + s, a.w + s}; }
static inline float4 float4_sub  (float4 a, float4 b) { return (float4){a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
static inline float4 float4_sub_s(float4 a, float  s) { return (float4){a.x - s, a.y - s, a.z - s, a.w - s}; }
static inline float4 float4_mul  (float4 a, float4 b) { return (float4){a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }
static inline float4 float4_mul_s(float4 a, float  s) { return (float4){a.x * s, a.y * s, a.z * s, a.w * s}; }
static inline float4 float4_div  (float4 a, float4 b) { return (float4){a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w}; }
static inline float4 float4_div_s(float4 a, float  s) { return (float4){a.x / s, a.y / s, a.z / s, a.w / s}; }
static inline float  float4_dot  (float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
static inline float  float4_mag2 (float4 v)           { return float4_dot(v, v); }
static inline float  float4_dist2(float4 a, float4 b) { return float4_mag2(float4_sub(a, b)); }

static inline float4 float4_frac(float4 v) {
	return (float4){v.x - floorf(v.x), v.y - floorf(v.y), v.z - floorf(v.z), v.w - floorf(v.w)};
}

static inline float4 float4_floor(float4 v) {
	return (float4){floorf(v.x), floorf(v.y), floorf(v.z), floorf(v.w)};
}

static inline float4 float4_ceil(float4 v) {
	return (float4){ceilf(v.x), ceilf(v.y), ceilf(v.z), ceilf(v.w)};
}

static inline float4 float4_abs(float4 v) {
	return (float4){fabsf(v.x), fabsf(v.y), fabsf(v.z), fabsf(v.w)};
}

static inline float4 float4_min(float4 a, float4 b) {
	return (float4){fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z), fminf(a.w, b.w)};
}

static inline float4 float4_max(float4 a, float4 b) {
	return (float4){fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z), fmaxf(a.w, b.w)};
}

static inline float float4_mag(float4 v) {
	return sqrtf(float4_mag2(v));
}

static inline float float4_dist(float4 a, float4 b) {
	return sqrtf(float4_dist2(a, b));
}

static inline float4 float4_norm(float4 v) {
	float mag = float4_mag(v);
	if (mag == 0.0f) {
		return (float4){0.0f, 0.0f, 0.0f, 0.0f};
	}
	return float4_div_s(v, mag);
}

// ============================================================================
// Quaternion operations (using float4)
// ============================================================================

// Conjugate (inverse for unit quaternions)
static inline float4 float4_quat_conjugate(float4 q) {
	return (float4){-q.x, -q.y, -q.z, q.w};
}

static inline float4 float4_quat_from_euler(float3 euler_xyz) {
	// Half angles
	float cx = cosf(euler_xyz.x * 0.5f);
	float sx = sinf(euler_xyz.x * 0.5f);
	float cy = cosf(euler_xyz.y * 0.5f);
	float sy = sinf(euler_xyz.y * 0.5f);
	float cz = cosf(euler_xyz.z * 0.5f);
	float sz = sinf(euler_xyz.z * 0.5f);

	// XYZ order
	return (float4){
		sx * cy * cz - cx * sy * sz,
		cx * sy * cz + sx * cy * sz,
		cx * cy * sz - sx * sy * cz,
		cx * cy * cz + sx * sy * sz
	};
}

static inline float4 float4_quat_from_axis_angle(float3 axis, float angle) {
	float half_angle = angle * 0.5f;
	float s = sinf(half_angle);
	return (float4){
		axis.x * s,
		axis.y * s,
		axis.z * s,
		cosf(half_angle)
	};
}

static inline float4 float4_quat_mul(float4 a, float4 b) {
	return (float4){
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
	};
}

static inline float3 float4_quat_rotate(float4 q, float3 v) {
	// v' = q * v * q^-1
	float3 qv = (float3){q.x, q.y, q.z};
	float3 uv = float3_cross(qv, v);
	float3 uuv = float3_cross(qv, uv);
	uv = float3_mul_s(uv, 2.0f * q.w);
	uuv = float3_mul_s(uuv, 2.0f);
	return float3_add(float3_add(v, uv), uuv);
}

// ============================================================================
// Matrix operations
// ============================================================================

static inline float4x4 float4x4_identity(void) {
	return (float4x4){{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	}};
}

static inline float4x4 float4x4_mul(float4x4 a, float4x4 b) {
	float4x4 result;

	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++) {
				sum += a.m[row * 4 + k] * b.m[k * 4 + col];
			}
			result.m[row * 4 + col] = sum;
		}
	}

	return result;
}

static inline float4x4 float4x4_transpose(float4x4 m) {
	return (float4x4){{
		m.m[0], m.m[4], m.m[8],  m.m[12],
		m.m[1], m.m[5], m.m[9],  m.m[13],
		m.m[2], m.m[6], m.m[10], m.m[14],
		m.m[3], m.m[7], m.m[11], m.m[15]
	}};
}

static inline float3 float4x4_transform_pt(float4x4 m, float3 pt) {
	float x = m.m[0] * pt.x + m.m[1] * pt.y + m.m[2]  * pt.z + m.m[3];
	float y = m.m[4] * pt.x + m.m[5] * pt.y + m.m[6]  * pt.z + m.m[7];
	float z = m.m[8] * pt.x + m.m[9] * pt.y + m.m[10] * pt.z + m.m[11];
	return (float3){x, y, z};
}

static inline float3 float4x4_transform_dir(float4x4 m, float3 dir) {
	float x = m.m[0] * dir.x + m.m[1] * dir.y + m.m[2]  * dir.z;
	float y = m.m[4] * dir.x + m.m[5] * dir.y + m.m[6]  * dir.z;
	float z = m.m[8] * dir.x + m.m[9] * dir.y + m.m[10] * dir.z;
	return (float3){x, y, z};
}

static inline float4 float4x4_transform_float4(float4x4 m, float4 v) {
	float x = m.m[0] * v.x + m.m[1] * v.y + m.m[2]  * v.z + m.m[3]  * v.w;
	float y = m.m[4] * v.x + m.m[5] * v.y + m.m[6]  * v.z + m.m[7]  * v.w;
	float z = m.m[8] * v.x + m.m[9] * v.y + m.m[10] * v.z + m.m[11] * v.w;
	float w = m.m[12] * v.x + m.m[13] * v.y + m.m[14] * v.z + m.m[15] * v.w;
	return (float4){x, y, z, w};
}

// Fast transforms using pre-transposed matrix
// With transposed layout, original columns are now at indices [0,1,2,3], [4,5,6,7], etc.
static inline float3 float4x4_transform_fast_pt(float4x4 mt, float3 pt) {
	// mt is transposed: mt.m[0..3] = original col0, mt.m[4..7] = original col1, etc.
	float x = mt.m[0] * pt.x + mt.m[4] * pt.y + mt.m[8]  * pt.z + mt.m[12];
	float y = mt.m[1] * pt.x + mt.m[5] * pt.y + mt.m[9]  * pt.z + mt.m[13];
	float z = mt.m[2] * pt.x + mt.m[6] * pt.y + mt.m[10] * pt.z + mt.m[14];
	return (float3){x, y, z};
}

static inline float3 float4x4_transform_fast_dir(float4x4 mt, float3 dir) {
	float x = mt.m[0] * dir.x + mt.m[4] * dir.y + mt.m[8]  * dir.z;
	float y = mt.m[1] * dir.x + mt.m[5] * dir.y + mt.m[9]  * dir.z;
	float z = mt.m[2] * dir.x + mt.m[6] * dir.y + mt.m[10] * dir.z;
	return (float3){x, y, z};
}

static inline float4 float4x4_transform_fast_float4(float4x4 mt, float4 v) {
	float4 result;
	result.x = mt.m[0] * v.x + mt.m[4] * v.y + mt.m[8]  * v.z + mt.m[12] * v.w;
	result.y = mt.m[1] * v.x + mt.m[5] * v.y + mt.m[9]  * v.z + mt.m[13] * v.w;
	result.z = mt.m[2] * v.x + mt.m[6] * v.y + mt.m[10] * v.z + mt.m[14] * v.w;
	result.w = mt.m[3] * v.x + mt.m[7] * v.y + mt.m[11] * v.z + mt.m[15] * v.w;
	return result;
}

// Float3s versions - same as float3 in fallback (no SIMD benefit)
static inline float3s float4x4_transform_fast_pt3s(float4x4 mt, float3s pt) {
	float3s result;
	result.x = mt.m[0] * pt.x + mt.m[4] * pt.y + mt.m[8]  * pt.z + mt.m[12];
	result.y = mt.m[1] * pt.x + mt.m[5] * pt.y + mt.m[9]  * pt.z + mt.m[13];
	result.z = mt.m[2] * pt.x + mt.m[6] * pt.y + mt.m[10] * pt.z + mt.m[14];
	return result;
}

static inline float3s float4x4_transform_fast_dir3s(float4x4 mt, float3s dir) {
	float3s result;
	result.x = mt.m[0] * dir.x + mt.m[4] * dir.y + mt.m[8]  * dir.z;
	result.y = mt.m[1] * dir.x + mt.m[5] * dir.y + mt.m[9]  * dir.z;
	result.z = mt.m[2] * dir.x + mt.m[6] * dir.y + mt.m[10] * dir.z;
	return result;
}

static inline float4x4 float4x4_t(float3 translation) {
	return (float4x4){{
		1.0f, 0.0f, 0.0f, translation.x,
		0.0f, 1.0f, 0.0f, translation.y,
		0.0f, 0.0f, 1.0f, translation.z,
		0.0f, 0.0f, 0.0f, 1.0f
	}};
}

static inline float4x4 float4x4_r(float4 quat) {
	// Normalize quaternion
	float4 q = float4_norm(quat);

	float xx = q.x * q.x;
	float yy = q.y * q.y;
	float zz = q.z * q.z;
	float xy = q.x * q.y;
	float xz = q.x * q.z;
	float yz = q.y * q.z;
	float wx = q.w * q.x;
	float wy = q.w * q.y;
	float wz = q.w * q.z;

	return (float4x4){{
		1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy), 0.0f,
		2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx), 0.0f,
		2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy), 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	}};
}

static inline float4x4 float4x4_s(float3 scale) {
	return (float4x4){{
		scale.x, 0.0f, 0.0f, 0.0f,
		0.0f, scale.y, 0.0f, 0.0f,
		0.0f, 0.0f, scale.z, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	}};
}

static inline float4x4 float4x4_trs(float3 translation, float4 rotation_quat, float3 scale) {
	// Normalize quaternion
	float4 q = float4_norm(rotation_quat);

	float xx = q.x * q.x;
	float yy = q.y * q.y;
	float zz = q.z * q.z;
	float xy = q.x * q.y;
	float xz = q.x * q.z;
	float yz = q.y * q.z;
	float wx = q.w * q.x;
	float wy = q.w * q.y;
	float wz = q.w * q.z;

	// Combined TRS matrix (row-major)
	return (float4x4){{
		scale.x * (1.0f - 2.0f * (yy + zz)), scale.x * (2.0f * (xy - wz)), scale.x * (2.0f * (xz + wy)), translation.x,
		scale.y * (2.0f * (xy + wz)), scale.y * (1.0f - 2.0f * (xx + zz)), scale.y * (2.0f * (yz - wx)), translation.y,
		scale.z * (2.0f * (xz - wy)), scale.z * (2.0f * (yz + wx)), scale.z * (1.0f - 2.0f * (xx + yy)), translation.z,
		0.0f, 0.0f, 0.0f, 1.0f
	}};
}

static inline float4x4 float4x4_lookat(float3 eye, float3 target, float3 up) {
	// Right-handed coordinate system
	float3 forward = float3_norm(float3_sub(target, eye));
	float3 right = float3_norm(float3_cross(forward, up));
	float3 actual_up = float3_cross(right, forward);

	// Row-major view matrix
	return (float4x4){{
		right.x, right.y, right.z, -float3_dot(right, eye),
		actual_up.x, actual_up.y, actual_up.z, -float3_dot(actual_up, eye),
		-forward.x, -forward.y, -forward.z, float3_dot(forward, eye),
		0.0f, 0.0f, 0.0f, 1.0f
	}};
}

static inline float4x4 float4x4_perspective(float fov_y, float aspect, float near_plane, float far_plane) {
	// Right-handed, zero-to-one depth, with Vulkan Y-flip built in
	float tan_half_fov = tanf(fov_y * 0.5f);

	// Row-major projection matrix
	return (float4x4){{
		1.0f / (aspect * tan_half_fov), 0.0f, 0.0f, 0.0f,
		0.0f, -1.0f / tan_half_fov, 0.0f, 0.0f,  // Negative Y for Vulkan flip
		0.0f, 0.0f, far_plane / (near_plane - far_plane), -(far_plane * near_plane) / (far_plane - near_plane),
		0.0f, 0.0f, -1.0f, 0.0f
	}};
}

static inline float4x4 float4x4_orthographic(float left, float right, float bottom, float top, float near_plane, float far_plane) {
	// Right-handed, zero-to-one depth, with Vulkan Y-flip built in
	// Row-major orthographic matrix
	return (float4x4){{
		2.0f / (right - left), 0.0f, 0.0f, -(right + left) / (right - left),
		0.0f, -2.0f / (top - bottom), 0.0f, (top + bottom) / (top - bottom),  // Negative Y for Vulkan flip
		0.0f, 0.0f, -1.0f / (far_plane - near_plane), -near_plane / (far_plane - near_plane),
		0.0f, 0.0f, 0.0f, 1.0f
	}};
}

// ============================================================================
// Larger functions (kept at bottom for readability)
// ============================================================================

static inline float4x4 float4x4_invert(float4x4 m) {
	float inv[16];
	float det;

	// Compute cofactors (standard matrix inversion formula)
	inv[0] = m.m[5]  * m.m[10] * m.m[15] -
	         m.m[5]  * m.m[11] * m.m[14] -
	         m.m[9]  * m.m[6]  * m.m[15] +
	         m.m[9]  * m.m[7]  * m.m[14] +
	         m.m[13] * m.m[6]  * m.m[11] -
	         m.m[13] * m.m[7]  * m.m[10];

	inv[4] = -m.m[4]  * m.m[10] * m.m[15] +
	          m.m[4]  * m.m[11] * m.m[14] +
	          m.m[8]  * m.m[6]  * m.m[15] -
	          m.m[8]  * m.m[7]  * m.m[14] -
	          m.m[12] * m.m[6]  * m.m[11] +
	          m.m[12] * m.m[7]  * m.m[10];

	inv[8] = m.m[4]  * m.m[9] * m.m[15] -
	         m.m[4]  * m.m[11] * m.m[13] -
	         m.m[8]  * m.m[5] * m.m[15] +
	         m.m[8]  * m.m[7] * m.m[13] +
	         m.m[12] * m.m[5] * m.m[11] -
	         m.m[12] * m.m[7] * m.m[9];

	inv[12] = -m.m[4]  * m.m[9] * m.m[14] +
	           m.m[4]  * m.m[10] * m.m[13] +
	           m.m[8]  * m.m[5] * m.m[14] -
	           m.m[8]  * m.m[6] * m.m[13] -
	           m.m[12] * m.m[5] * m.m[10] +
	           m.m[12] * m.m[6] * m.m[9];

	inv[1] = -m.m[1]  * m.m[10] * m.m[15] +
	          m.m[1]  * m.m[11] * m.m[14] +
	          m.m[9]  * m.m[2] * m.m[15] -
	          m.m[9]  * m.m[3] * m.m[14] -
	          m.m[13] * m.m[2] * m.m[11] +
	          m.m[13] * m.m[3] * m.m[10];

	inv[5] = m.m[0]  * m.m[10] * m.m[15] -
	         m.m[0]  * m.m[11] * m.m[14] -
	         m.m[8]  * m.m[2] * m.m[15] +
	         m.m[8]  * m.m[3] * m.m[14] +
	         m.m[12] * m.m[2] * m.m[11] -
	         m.m[12] * m.m[3] * m.m[10];

	inv[9] = -m.m[0]  * m.m[9] * m.m[15] +
	          m.m[0]  * m.m[11] * m.m[13] +
	          m.m[8]  * m.m[1] * m.m[15] -
	          m.m[8]  * m.m[3] * m.m[13] -
	          m.m[12] * m.m[1] * m.m[11] +
	          m.m[12] * m.m[3] * m.m[9];

	inv[13] = m.m[0]  * m.m[9] * m.m[14] -
	          m.m[0]  * m.m[10] * m.m[13] -
	          m.m[8]  * m.m[1] * m.m[14] +
	          m.m[8]  * m.m[2] * m.m[13] +
	          m.m[12] * m.m[1] * m.m[10] -
	          m.m[12] * m.m[2] * m.m[9];

	inv[2] = m.m[1]  * m.m[6] * m.m[15] -
	         m.m[1]  * m.m[7] * m.m[14] -
	         m.m[5]  * m.m[2] * m.m[15] +
	         m.m[5]  * m.m[3] * m.m[14] +
	         m.m[13] * m.m[2] * m.m[7] -
	         m.m[13] * m.m[3] * m.m[6];

	inv[6] = -m.m[0]  * m.m[6] * m.m[15] +
	          m.m[0]  * m.m[7] * m.m[14] +
	          m.m[4]  * m.m[2] * m.m[15] -
	          m.m[4]  * m.m[3] * m.m[14] -
	          m.m[12] * m.m[2] * m.m[7] +
	          m.m[12] * m.m[3] * m.m[6];

	inv[10] = m.m[0]  * m.m[5] * m.m[15] -
	          m.m[0]  * m.m[7] * m.m[13] -
	          m.m[4]  * m.m[1] * m.m[15] +
	          m.m[4]  * m.m[3] * m.m[13] +
	          m.m[12] * m.m[1] * m.m[7] -
	          m.m[12] * m.m[3] * m.m[5];

	inv[14] = -m.m[0]  * m.m[5] * m.m[14] +
	           m.m[0]  * m.m[6] * m.m[13] +
	           m.m[4]  * m.m[1] * m.m[14] -
	           m.m[4]  * m.m[2] * m.m[13] -
	           m.m[12] * m.m[1] * m.m[6] +
	           m.m[12] * m.m[2] * m.m[5];

	inv[3] = -m.m[1] * m.m[6] * m.m[11] +
	          m.m[1] * m.m[7] * m.m[10] +
	          m.m[5] * m.m[2] * m.m[11] -
	          m.m[5] * m.m[3] * m.m[10] -
	          m.m[9] * m.m[2] * m.m[7] +
	          m.m[9] * m.m[3] * m.m[6];

	inv[7] = m.m[0] * m.m[6] * m.m[11] -
	         m.m[0] * m.m[7] * m.m[10] -
	         m.m[4] * m.m[2] * m.m[11] +
	         m.m[4] * m.m[3] * m.m[10] +
	         m.m[8] * m.m[2] * m.m[7] -
	         m.m[8] * m.m[3] * m.m[6];

	inv[11] = -m.m[0] * m.m[5] * m.m[11] +
	           m.m[0] * m.m[7] * m.m[9] +
	           m.m[4] * m.m[1] * m.m[11] -
	           m.m[4] * m.m[3] * m.m[9] -
	           m.m[8] * m.m[1] * m.m[7] +
	           m.m[8] * m.m[3] * m.m[5];

	inv[15] = m.m[0] * m.m[5] * m.m[10] -
	          m.m[0] * m.m[6] * m.m[9] -
	          m.m[4] * m.m[1] * m.m[10] +
	          m.m[4] * m.m[2] * m.m[9] +
	          m.m[8] * m.m[1] * m.m[6] -
	          m.m[8] * m.m[2] * m.m[5];

	det = m.m[0] * inv[0] + m.m[1] * inv[4] + m.m[2] * inv[8] + m.m[3] * inv[12];

	if (det == 0.0f) {
		return float4x4_identity();
	}

	det = 1.0f / det;

	float4x4 result;
	for (int i = 0; i < 16; i++) {
		result.m[i] = inv[i] * det;
	}

	return result;
}

#ifdef __cplusplus
}
#endif


#endif // FLOAT_MATH_USE_FALLBACK

// ============================================================================
// SSE implementation
// ============================================================================
#if defined(FLOAT_MATH_USE_SSE)


#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <xmmintrin.h> // SSE
#include <emmintrin.h> // SSE2
#include <smmintrin.h> // SSE4.1

#if defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
	#define FM_ALIGN_PREFIX(x) __declspec(align(x))
	#define FM_ALIGN_SUFFIX(x)
#else
	#define FM_ALIGN_PREFIX(x)
	#define FM_ALIGN_SUFFIX(x) __attribute__((aligned(x)))
#endif

// ============================================================================
// Vector types (SSE optimized)
// ============================================================================

typedef struct { float x, y; } float2;

// float3: 12 bytes, matches user expectation for size
typedef struct { float x, y, z; } float3;

// float3s: 16 bytes, SIMD-aligned for SSE operations
typedef FM_ALIGN_PREFIX(16) union {
	struct { float x, y, z, _pad; };
	float v[4];
	__m128 simd;
} FM_ALIGN_SUFFIX(16) float3s;

// float4: 16 bytes, SIMD-aligned
typedef FM_ALIGN_PREFIX(16) union {
	struct { float x, y, z, w; };
	struct { float r, g, b, a; };
	float v[4];
	__m128 simd;
} FM_ALIGN_SUFFIX(16) float4;

// Matrix type (row-major for direct shader compatibility)
typedef FM_ALIGN_PREFIX(16) union {
	float m[16]; // Row-major: [m00, m01, m02, m03, m10, m11, ...]
	__m128 rows[4];
} FM_ALIGN_SUFFIX(16) float4x4;

// ============================================================================
// Conversions between float3 and float3s
// ============================================================================

static inline float3 float3s_to_float3(float3s v) {
	return (float3){v.x, v.y, v.z};
}

static inline float3s float3_to_float3s(float3 v) {
	float3s result;
	result.simd = _mm_set_ps(0.0f, v.z, v.y, v.x);
	return result;
}

// ============================================================================
// SSE Helper functions
// ============================================================================

static inline float3s m128_to_float3s(__m128 v) {
	float3s result;
	result.simd = v;
	return result;
}

static inline float4 m128_to_float4(__m128 v) {
	float4 result;
	result.simd = v;
	return result;
}

// ============================================================================
// Float2 operations (scalar - SSE not beneficial for 2 floats)
// ============================================================================

static inline float2 float2_add  (float2 a, float2 b) { return (float2){a.x + b.x, a.y + b.y}; }
static inline float2 float2_add_s(float2 a, float  s) { return (float2){a.x + s, a.y + s}; }
static inline float2 float2_sub  (float2 a, float2 b) { return (float2){a.x - b.x, a.y - b.y}; }
static inline float2 float2_sub_s(float2 a, float  s) { return (float2){a.x - s, a.y - s}; }
static inline float2 float2_mul  (float2 a, float2 b) { return (float2){a.x * b.x, a.y * b.y}; }
static inline float2 float2_mul_s(float2 a, float  s) { return (float2){a.x * s, a.y * s}; }
static inline float2 float2_div  (float2 a, float2 b) { return (float2){a.x / b.x, a.y / b.y}; }
static inline float2 float2_div_s(float2 a, float  s) { return (float2){a.x / s, a.y / s}; }
static inline float  float2_dot  (float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
static inline float  float2_mag2 (float2 v)           { return float2_dot(v, v); }
static inline float  float2_dist2(float2 a, float2 b) { return float2_mag2(float2_sub(a, b)); }

static inline float2 float2_frac(float2 v) {
	return (float2){v.x - floorf(v.x), v.y - floorf(v.y)};
}

static inline float2 float2_floor(float2 v) {
	return (float2){floorf(v.x), floorf(v.y)};
}

static inline float2 float2_ceil(float2 v) {
	return (float2){ceilf(v.x), ceilf(v.y)};
}

static inline float2 float2_abs(float2 v) {
	return (float2){fabsf(v.x), fabsf(v.y)};
}

static inline float2 float2_min(float2 a, float2 b) {
	return (float2){fminf(a.x, b.x), fminf(a.y, b.y)};
}

static inline float2 float2_max(float2 a, float2 b) {
	return (float2){fmaxf(a.x, b.x), fmaxf(a.y, b.y)};
}

static inline float float2_mag(float2 v) {
	return sqrtf(float2_mag2(v));
}

static inline float float2_dist(float2 a, float2 b) {
	return sqrtf(float2_dist2(a, b));
}

static inline float2 float2_norm(float2 v) {
	float mag = float2_mag(v);
	if (mag == 0.0f) {
		return (float2){0.0f, 0.0f};
	}
	return float2_div_s(v, mag);
}

// ============================================================================
// Float3 operations (scalar - 12 bytes, no padding)
// ============================================================================

static inline float3 float3_add  (float3 a, float3 b) { return (float3){a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline float3 float3_add_s(float3 a, float  s) { return (float3){a.x + s, a.y + s, a.z + s}; }
static inline float3 float3_sub  (float3 a, float3 b) { return (float3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline float3 float3_sub_s(float3 a, float  s) { return (float3){a.x - s, a.y - s, a.z - s}; }
static inline float3 float3_mul  (float3 a, float3 b) { return (float3){a.x * b.x, a.y * b.y, a.z * b.z}; }
static inline float3 float3_mul_s(float3 a, float  s) { return (float3){a.x * s, a.y * s, a.z * s}; }
static inline float3 float3_div  (float3 a, float3 b) { return (float3){a.x / b.x, a.y / b.y, a.z / b.z}; }
static inline float3 float3_div_s(float3 a, float  s) { return (float3){a.x / s, a.y / s, a.z / s}; }
static inline float  float3_dot  (float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float  float3_mag2 (float3 v)           { return float3_dot(v, v); }
static inline float  float3_dist2(float3 a, float3 b) { return float3_mag2(float3_sub(a, b)); }
static inline float3 float3_cross(float3 a, float3 b) {
	return (float3){
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
}

static inline float3 float3_frac(float3 v) {
	return (float3){v.x - floorf(v.x), v.y - floorf(v.y), v.z - floorf(v.z)};
}

static inline float3 float3_floor(float3 v) {
	return (float3){floorf(v.x), floorf(v.y), floorf(v.z)};
}

static inline float3 float3_ceil(float3 v) {
	return (float3){ceilf(v.x), ceilf(v.y), ceilf(v.z)};
}

static inline float3 float3_abs(float3 v) {
	return (float3){fabsf(v.x), fabsf(v.y), fabsf(v.z)};
}

static inline float3 float3_min(float3 a, float3 b) {
	return (float3){fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z)};
}

static inline float3 float3_max(float3 a, float3 b) {
	return (float3){fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z)};
}

static inline float float3_mag(float3 v) {
	return sqrtf(float3_mag2(v));
}

static inline float float3_dist(float3 a, float3 b) {
	return sqrtf(float3_dist2(a, b));
}

static inline float3 float3_norm(float3 v) {
	float mag = float3_mag(v);
	if (mag == 0.0f) {
		return (float3){0.0f, 0.0f, 0.0f};
	}
	return float3_div_s(v, mag);
}

// ============================================================================
// Float3s operations (SSE accelerated - 16 bytes, SIMD aligned)
// ============================================================================

static inline float3s float3s_add(float3s a, float3s b) {
	return m128_to_float3s(_mm_add_ps(a.simd, b.simd));
}

static inline float3s float3s_add_s(float3s a, float s) {
	return m128_to_float3s(_mm_add_ps(a.simd, _mm_set1_ps(s)));
}

static inline float3s float3s_sub(float3s a, float3s b) {
	return m128_to_float3s(_mm_sub_ps(a.simd, b.simd));
}

static inline float3s float3s_sub_s(float3s a, float s) {
	return m128_to_float3s(_mm_sub_ps(a.simd, _mm_set1_ps(s)));
}

static inline float3s float3s_mul(float3s a, float3s b) {
	return m128_to_float3s(_mm_mul_ps(a.simd, b.simd));
}

static inline float3s float3s_mul_s(float3s a, float s) {
	return m128_to_float3s(_mm_mul_ps(a.simd, _mm_set1_ps(s)));
}

static inline float3s float3s_div(float3s a, float3s b) {
	return m128_to_float3s(_mm_div_ps(a.simd, b.simd));
}

static inline float3s float3s_div_s(float3s a, float s) {
	return m128_to_float3s(_mm_div_ps(a.simd, _mm_set1_ps(s)));
}

static inline float float3s_dot(float3s a, float3s b) {
	// SSE4.1 dot product: 0x71 = multiply xyz, store in x
	return _mm_cvtss_f32(_mm_dp_ps(a.simd, b.simd, 0x71));
}

static inline float float3s_mag2(float3s v) {
	return float3s_dot(v, v);
}

static inline float float3s_dist2(float3s a, float3s b) {
	return float3s_mag2(float3s_sub(a, b));
}

static inline float3s float3s_cross(float3s a, float3s b) {
	// Cross product using shuffles: a.yzx * b.zxy - a.zxy * b.yzx
	__m128 a_yzx = _mm_shuffle_ps(a.simd, a.simd, _MM_SHUFFLE(3, 0, 2, 1));
	__m128 b_zxy = _mm_shuffle_ps(b.simd, b.simd, _MM_SHUFFLE(3, 1, 0, 2));
	__m128 a_zxy = _mm_shuffle_ps(a.simd, a.simd, _MM_SHUFFLE(3, 1, 0, 2));
	__m128 b_yzx = _mm_shuffle_ps(b.simd, b.simd, _MM_SHUFFLE(3, 0, 2, 1));
	return m128_to_float3s(_mm_sub_ps(_mm_mul_ps(a_yzx, b_zxy), _mm_mul_ps(a_zxy, b_yzx)));
}

static inline float3s float3s_frac(float3s v) {
	__m128 floor_v = _mm_floor_ps(v.simd);
	return m128_to_float3s(_mm_sub_ps(v.simd, floor_v));
}

static inline float3s float3s_floor(float3s v) {
	return m128_to_float3s(_mm_floor_ps(v.simd));
}

static inline float3s float3s_ceil(float3s v) {
	return m128_to_float3s(_mm_ceil_ps(v.simd));
}

static inline float3s float3s_abs(float3s v) {
	__m128 sign_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
	return m128_to_float3s(_mm_and_ps(v.simd, sign_mask));
}

static inline float3s float3s_min(float3s a, float3s b) {
	return m128_to_float3s(_mm_min_ps(a.simd, b.simd));
}

static inline float3s float3s_max(float3s a, float3s b) {
	return m128_to_float3s(_mm_max_ps(a.simd, b.simd));
}

static inline float float3s_mag(float3s v) {
	__m128 dp = _mm_dp_ps(v.simd, v.simd, 0x71);
	return _mm_cvtss_f32(_mm_sqrt_ss(dp));
}

static inline float float3s_dist(float3s a, float3s b) {
	__m128 diff = _mm_sub_ps(a.simd, b.simd);
	__m128 dp = _mm_dp_ps(diff, diff, 0x71);
	return _mm_cvtss_f32(_mm_sqrt_ss(dp));
}

static inline float3s float3s_norm(float3s v) {
	__m128 dp = _mm_dp_ps(v.simd, v.simd, 0x7F);
	if (_mm_cvtss_f32(dp) == 0.0f) {
		float3s zero = {0};
		return zero;
	}
	__m128 inv_mag = _mm_rsqrt_ps(dp);
	__m128 half = _mm_set1_ps(0.5f);
	__m128 three = _mm_set1_ps(3.0f);
	inv_mag = _mm_mul_ps(_mm_mul_ps(half, inv_mag),
	                     _mm_sub_ps(three, _mm_mul_ps(_mm_mul_ps(dp, inv_mag), inv_mag)));
	return m128_to_float3s(_mm_mul_ps(v.simd, inv_mag));
}

// ============================================================================
// Float4 operations (SSE accelerated)
// ============================================================================

static inline float4 float4_add(float4 a, float4 b) {
	return m128_to_float4(_mm_add_ps(a.simd, b.simd));
}

static inline float4 float4_add_s(float4 a, float s) {
	return m128_to_float4(_mm_add_ps(a.simd, _mm_set1_ps(s)));
}

static inline float4 float4_sub(float4 a, float4 b) {
	return m128_to_float4(_mm_sub_ps(a.simd, b.simd));
}

static inline float4 float4_sub_s(float4 a, float s) {
	return m128_to_float4(_mm_sub_ps(a.simd, _mm_set1_ps(s)));
}

static inline float4 float4_mul(float4 a, float4 b) {
	return m128_to_float4(_mm_mul_ps(a.simd, b.simd));
}

static inline float4 float4_mul_s(float4 a, float s) {
	return m128_to_float4(_mm_mul_ps(a.simd, _mm_set1_ps(s)));
}

static inline float4 float4_div(float4 a, float4 b) {
	return m128_to_float4(_mm_div_ps(a.simd, b.simd));
}

static inline float4 float4_div_s(float4 a, float s) {
	return m128_to_float4(_mm_div_ps(a.simd, _mm_set1_ps(s)));
}

static inline float float4_dot(float4 a, float4 b) {
	// SSE4.1 dot product: 0xF1 = multiply all 4, store in x
	return _mm_cvtss_f32(_mm_dp_ps(a.simd, b.simd, 0xF1));
}

static inline float float4_mag2(float4 v) {
	return float4_dot(v, v);
}

static inline float float4_dist2(float4 a, float4 b) {
	return float4_mag2(float4_sub(a, b));
}

static inline float4 float4_frac(float4 v) {
	__m128 floor_v = _mm_floor_ps(v.simd);
	return m128_to_float4(_mm_sub_ps(v.simd, floor_v));
}

static inline float4 float4_floor(float4 v) {
	return m128_to_float4(_mm_floor_ps(v.simd));
}

static inline float4 float4_ceil(float4 v) {
	return m128_to_float4(_mm_ceil_ps(v.simd));
}

static inline float4 float4_abs(float4 v) {
	__m128 sign_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
	return m128_to_float4(_mm_and_ps(v.simd, sign_mask));
}

static inline float4 float4_min(float4 a, float4 b) {
	return m128_to_float4(_mm_min_ps(a.simd, b.simd));
}

static inline float4 float4_max(float4 a, float4 b) {
	return m128_to_float4(_mm_max_ps(a.simd, b.simd));
}

static inline float float4_mag(float4 v) {
	__m128 dp = _mm_dp_ps(v.simd, v.simd, 0xF1);
	return _mm_cvtss_f32(_mm_sqrt_ss(dp));
}

static inline float float4_dist(float4 a, float4 b) {
	__m128 diff = _mm_sub_ps(a.simd, b.simd);
	__m128 dp = _mm_dp_ps(diff, diff, 0xF1);
	return _mm_cvtss_f32(_mm_sqrt_ss(dp));
}

static inline float4 float4_norm(float4 v) {
	__m128 dp = _mm_dp_ps(v.simd, v.simd, 0xFF);
	if (_mm_cvtss_f32(dp) == 0.0f) {
		float4 zero = {0};
		return zero;
	}
	__m128 inv_mag = _mm_rsqrt_ps(dp);
	// Newton-Raphson refinement
	__m128 half = _mm_set1_ps(0.5f);
	__m128 three = _mm_set1_ps(3.0f);
	inv_mag = _mm_mul_ps(_mm_mul_ps(half, inv_mag),
	                     _mm_sub_ps(three, _mm_mul_ps(_mm_mul_ps(dp, inv_mag), inv_mag)));
	return m128_to_float4(_mm_mul_ps(v.simd, inv_mag));
}

// ============================================================================
// Quaternion operations (using float4)
// ============================================================================

// Conjugate (inverse for unit quaternions)
static inline float4 float4_quat_conjugate(float4 q) {
	// Negate xyz, keep w
	__m128 sign = _mm_set_ps(1.0f, -1.0f, -1.0f, -1.0f);
	return m128_to_float4(_mm_mul_ps(q.simd, sign));
}

static inline float4 float4_quat_from_euler(float3 euler_xyz) {
	// Half angles
	float cx = cosf(euler_xyz.x * 0.5f);
	float sx = sinf(euler_xyz.x * 0.5f);
	float cy = cosf(euler_xyz.y * 0.5f);
	float sy = sinf(euler_xyz.y * 0.5f);
	float cz = cosf(euler_xyz.z * 0.5f);
	float sz = sinf(euler_xyz.z * 0.5f);

	// XYZ order
	float4 result;
	result.x = sx * cy * cz - cx * sy * sz;
	result.y = cx * sy * cz + sx * cy * sz;
	result.z = cx * cy * sz - sx * sy * cz;
	result.w = cx * cy * cz + sx * sy * sz;
	return result;
}

static inline float4 float4_quat_from_axis_angle(float3 axis, float angle) {
	float half_angle = angle * 0.5f;
	float s = sinf(half_angle);
	float4 result;
	result.x = axis.x * s;
	result.y = axis.y * s;
	result.z = axis.z * s;
	result.w = cosf(half_angle);
	return result;
}

static inline float4 float4_quat_mul(float4 a, float4 b) {
	// Standard quaternion multiplication formula
	float4 result;
	result.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	result.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	result.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
	result.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	return result;
}

static inline float3 float4_quat_rotate(float4 q, float3 v) {
	// v' = q * v * q^-1
	float3 qv = {q.x, q.y, q.z};
	float3 uv = float3_cross(qv, v);
	float3 uuv = float3_cross(qv, uv);
	uv = float3_mul_s(uv, 2.0f * q.w);
	uuv = float3_mul_s(uuv, 2.0f);
	return float3_add(float3_add(v, uv), uuv);
}

// ============================================================================
// Matrix operations
// ============================================================================

static inline float4x4 float4x4_identity(void) {
	float4x4 result;
	result.rows[0] = _mm_set_ps(0.0f, 0.0f, 0.0f, 1.0f);
	result.rows[1] = _mm_set_ps(0.0f, 0.0f, 1.0f, 0.0f);
	result.rows[2] = _mm_set_ps(0.0f, 1.0f, 0.0f, 0.0f);
	result.rows[3] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
	return result;
}

static inline float4x4 float4x4_mul(float4x4 a, float4x4 b) {
	// DirectXMath-style: broadcast each element and multiply by corresponding row of B
	// This avoids the high-latency _mm_dp_ps instruction
	float4x4 result;

#if defined(__AVX2__) && defined(__FMA__)
	// AVX2+FMA: Use broadcast and fused multiply-add
	for (int i = 0; i < 4; i++) {
		__m128 vx = _mm_broadcastss_ps(a.rows[i]);
		__m128 vy = _mm_permute_ps(a.rows[i], _MM_SHUFFLE(1, 1, 1, 1));
		__m128 vz = _mm_permute_ps(a.rows[i], _MM_SHUFFLE(2, 2, 2, 2));
		__m128 vw = _mm_permute_ps(a.rows[i], _MM_SHUFFLE(3, 3, 3, 3));

		__m128 r = _mm_mul_ps(vx, b.rows[0]);
		r = _mm_fmadd_ps(vy, b.rows[1], r);
		r = _mm_fmadd_ps(vz, b.rows[2], r);
		r = _mm_fmadd_ps(vw, b.rows[3], r);
		result.rows[i] = r;
	}
#else
	// SSE: Use shuffle for broadcast, binary add to reduce error accumulation
	for (int i = 0; i < 4; i++) {
		__m128 vx = _mm_shuffle_ps(a.rows[i], a.rows[i], _MM_SHUFFLE(0, 0, 0, 0));
		__m128 vy = _mm_shuffle_ps(a.rows[i], a.rows[i], _MM_SHUFFLE(1, 1, 1, 1));
		__m128 vz = _mm_shuffle_ps(a.rows[i], a.rows[i], _MM_SHUFFLE(2, 2, 2, 2));
		__m128 vw = _mm_shuffle_ps(a.rows[i], a.rows[i], _MM_SHUFFLE(3, 3, 3, 3));

		vx = _mm_mul_ps(vx, b.rows[0]);
		vy = _mm_mul_ps(vy, b.rows[1]);
		vz = _mm_mul_ps(vz, b.rows[2]);
		vw = _mm_mul_ps(vw, b.rows[3]);

		// Binary add pattern to reduce cumulative errors (like DirectXMath)
		vx = _mm_add_ps(vx, vz);
		vy = _mm_add_ps(vy, vw);
		result.rows[i] = _mm_add_ps(vx, vy);
	}
#endif

	return result;
}

static inline float4x4 float4x4_transpose(float4x4 m) {
	float4x4 result;
	result.rows[0] = m.rows[0];
	result.rows[1] = m.rows[1];
	result.rows[2] = m.rows[2];
	result.rows[3] = m.rows[3];
	_MM_TRANSPOSE4_PS(result.rows[0], result.rows[1], result.rows[2], result.rows[3]);
	return result;
}

static inline float3 float4x4_transform_pt(float4x4 m, float3 pt) {
	// Transpose then use broadcast-multiply-add pattern
	__m128 r0 = m.rows[0], r1 = m.rows[1], r2 = m.rows[2], r3 = m.rows[3];
	_MM_TRANSPOSE4_PS(r0, r1, r2, r3);

	__m128 v = _mm_set_ps(1.0f, pt.z, pt.y, pt.x);

#if defined(__AVX2__) && defined(__FMA__)
	__m128 vx = _mm_broadcastss_ps(v);
	__m128 vy = _mm_permute_ps(v, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vz = _mm_permute_ps(v, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vw = _mm_permute_ps(v, _MM_SHUFFLE(3, 3, 3, 3));

	__m128 r = _mm_mul_ps(vx, r0);
	r = _mm_fmadd_ps(vy, r1, r);
	r = _mm_fmadd_ps(vz, r2, r);
	r = _mm_fmadd_ps(vw, r3, r);
#else
	__m128 vx = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 vy = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vz = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vw = _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));

	vx = _mm_mul_ps(vx, r0);
	vy = _mm_mul_ps(vy, r1);
	vz = _mm_mul_ps(vz, r2);
	vw = _mm_mul_ps(vw, r3);
	__m128 r = _mm_add_ps(_mm_add_ps(vx, vz), _mm_add_ps(vy, vw));
#endif

	float4 f4;
	f4.simd = r;
	return (float3){f4.x, f4.y, f4.z};
}

static inline float3 float4x4_transform_dir(float4x4 m, float3 dir) {
	// Transpose then use broadcast-multiply-add pattern (no translation)
	__m128 r0 = m.rows[0], r1 = m.rows[1], r2 = m.rows[2], r3 = m.rows[3];
	_MM_TRANSPOSE4_PS(r0, r1, r2, r3);

	__m128 v = _mm_set_ps(0.0f, dir.z, dir.y, dir.x);

#if defined(__AVX2__) && defined(__FMA__)
	__m128 vx = _mm_broadcastss_ps(v);
	__m128 vy = _mm_permute_ps(v, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vz = _mm_permute_ps(v, _MM_SHUFFLE(2, 2, 2, 2));

	__m128 r = _mm_mul_ps(vx, r0);
	r = _mm_fmadd_ps(vy, r1, r);
	r = _mm_fmadd_ps(vz, r2, r);
#else
	__m128 vx = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 vy = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vz = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));

	vx = _mm_mul_ps(vx, r0);
	vy = _mm_mul_ps(vy, r1);
	vz = _mm_mul_ps(vz, r2);
	__m128 r = _mm_add_ps(_mm_add_ps(vx, vz), vy);
#endif

	float4 f4;
	f4.simd = r;
	return (float3){f4.x, f4.y, f4.z};
}

static inline float4 float4x4_transform_float4(float4x4 m, float4 v) {
	// Transpose then use broadcast-multiply-add pattern
	__m128 r0 = m.rows[0], r1 = m.rows[1], r2 = m.rows[2], r3 = m.rows[3];
	_MM_TRANSPOSE4_PS(r0, r1, r2, r3);

#if defined(__AVX2__) && defined(__FMA__)
	__m128 vx = _mm_broadcastss_ps(v.simd);
	__m128 vy = _mm_permute_ps(v.simd, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vz = _mm_permute_ps(v.simd, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vw = _mm_permute_ps(v.simd, _MM_SHUFFLE(3, 3, 3, 3));

	__m128 r = _mm_mul_ps(vx, r0);
	r = _mm_fmadd_ps(vy, r1, r);
	r = _mm_fmadd_ps(vz, r2, r);
	r = _mm_fmadd_ps(vw, r3, r);
#else
	__m128 vx = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 vy = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vz = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vw = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(3, 3, 3, 3));

	vx = _mm_mul_ps(vx, r0);
	vy = _mm_mul_ps(vy, r1);
	vz = _mm_mul_ps(vz, r2);
	vw = _mm_mul_ps(vw, r3);
	__m128 r = _mm_add_ps(_mm_add_ps(vx, vz), _mm_add_ps(vy, vw));
#endif

	return m128_to_float4(r);
}

// Fast transforms using pre-transposed matrix (columns stored in rows[0..3])
#if defined(__AVX2__) && defined(__FMA__)

static inline float3 float4x4_transform_fast_pt(float4x4 mt, float3 pt) {
	__m128 v = _mm_set_ps(0.0f, pt.z, pt.y, pt.x);

	__m128 vz = _mm_permute_ps(v, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_permute_ps(v, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_broadcastss_ps(v);

	__m128 r = _mm_fmadd_ps(vz, mt.rows[2], mt.rows[3]);
	r = _mm_fmadd_ps(vy, mt.rows[1], r);
	r = _mm_fmadd_ps(vx, mt.rows[0], r);

	float4 f4;
	f4.simd = r;
	return (float3){f4.x, f4.y, f4.z};
}

static inline float3 float4x4_transform_fast_dir(float4x4 mt, float3 dir) {
	__m128 v = _mm_set_ps(0.0f, dir.z, dir.y, dir.x);
	__m128 vz = _mm_permute_ps(v, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_permute_ps(v, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_broadcastss_ps(v);

	__m128 r = _mm_mul_ps(vz, mt.rows[2]);
	r = _mm_fmadd_ps(vy, mt.rows[1], r);
	r = _mm_fmadd_ps(vx, mt.rows[0], r);

	float4 f4;
	f4.simd = r;
	return (float3){f4.x, f4.y, f4.z};
}

static inline float4 float4x4_transform_fast_float4(float4x4 mt, float4 v) {
	__m128 vz = _mm_permute_ps(v.simd, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_permute_ps(v.simd, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_broadcastss_ps(v.simd);
	__m128 vw = _mm_permute_ps(v.simd, _MM_SHUFFLE(3, 3, 3, 3));

	__m128 r = _mm_fmadd_ps(vz, mt.rows[2], _mm_mul_ps(vw, mt.rows[3]));
	r = _mm_fmadd_ps(vy, mt.rows[1], r);
	r = _mm_fmadd_ps(vx, mt.rows[0], r);
	return m128_to_float4(r);
}

static inline float3s float4x4_transform_fast_pt3s(float4x4 mt, float3s pt) {
	__m128 vz = _mm_permute_ps(pt.simd, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_permute_ps(pt.simd, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_broadcastss_ps(pt.simd);
	__m128 r = _mm_fmadd_ps(vz, mt.rows[2], mt.rows[3]);
	r = _mm_fmadd_ps(vy, mt.rows[1], r);
	r = _mm_fmadd_ps(vx, mt.rows[0], r);
	return m128_to_float3s(r);
}

static inline float3s float4x4_transform_fast_dir3s(float4x4 mt, float3s dir) {
	__m128 vz = _mm_permute_ps(dir.simd, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_permute_ps(dir.simd, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_broadcastss_ps(dir.simd);
	__m128 r = _mm_mul_ps(vz, mt.rows[2]);
	r = _mm_fmadd_ps(vy, mt.rows[1], r);
	r = _mm_fmadd_ps(vx, mt.rows[0], r);
	return m128_to_float3s(r);
}

#elif defined(__FMA__)

static inline float3 float4x4_transform_fast_pt(float4x4 mt, float3 pt) {
	__m128 v = _mm_set_ps(0.0f, pt.z, pt.y, pt.x);
	__m128 vz = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));

	__m128 r = _mm_fmadd_ps(vz, mt.rows[2], mt.rows[3]);
	r = _mm_fmadd_ps(vy, mt.rows[1], r);
	r = _mm_fmadd_ps(vx, mt.rows[0], r);

	float4 f4;
	f4.simd = r;
	return (float3){f4.x, f4.y, f4.z};
}

static inline float3 float4x4_transform_fast_dir(float4x4 mt, float3 dir) {
	__m128 v = _mm_set_ps(0.0f, dir.z, dir.y, dir.x);
	__m128 vz = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));

	__m128 r = _mm_mul_ps(vz, mt.rows[2]);
	r = _mm_fmadd_ps(vy, mt.rows[1], r);
	r = _mm_fmadd_ps(vx, mt.rows[0], r);

	float4 f4;
	f4.simd = r;
	return (float3){f4.x, f4.y, f4.z};
}

static inline float4 float4x4_transform_fast_float4(float4x4 mt, float4 v) {
	__m128 vz = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 vw = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(3, 3, 3, 3));

	__m128 r = _mm_fmadd_ps(vz, mt.rows[2], _mm_mul_ps(vw, mt.rows[3]));
	r = _mm_fmadd_ps(vy, mt.rows[1], r);
	r = _mm_fmadd_ps(vx, mt.rows[0], r);
	return m128_to_float4(r);
}

static inline float3s float4x4_transform_fast_pt3s(float4x4 mt, float3s pt) {
	__m128 vz = _mm_shuffle_ps(pt.simd, pt.simd, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_shuffle_ps(pt.simd, pt.simd, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_shuffle_ps(pt.simd, pt.simd, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 r = _mm_fmadd_ps(vz, mt.rows[2], mt.rows[3]);
	r = _mm_fmadd_ps(vy, mt.rows[1], r);
	r = _mm_fmadd_ps(vx, mt.rows[0], r);
	return m128_to_float3s(r);
}

static inline float3s float4x4_transform_fast_dir3s(float4x4 mt, float3s dir) {
	__m128 vz = _mm_shuffle_ps(dir.simd, dir.simd, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_shuffle_ps(dir.simd, dir.simd, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_shuffle_ps(dir.simd, dir.simd, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 r = _mm_mul_ps(vz, mt.rows[2]);
	r = _mm_fmadd_ps(vy, mt.rows[1], r);
	r = _mm_fmadd_ps(vx, mt.rows[0], r);
	return m128_to_float3s(r);
}

#else
// SSE4.1 fallback (no FMA)

static inline float3 float4x4_transform_fast_pt(float4x4 mt, float3 pt) {
	__m128 v = _mm_set_ps(0.0f, pt.z, pt.y, pt.x);
	__m128 vx = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 vy = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vz = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));

	__m128 r = _mm_add_ps(_mm_mul_ps(mt.rows[0], vx), mt.rows[3]);
	r = _mm_add_ps(r, _mm_mul_ps(mt.rows[1], vy));
	r = _mm_add_ps(r, _mm_mul_ps(mt.rows[2], vz));

	float4 f4;
	f4.simd = r;
	return (float3){f4.x, f4.y, f4.z};
}

static inline float3 float4x4_transform_fast_dir(float4x4 mt, float3 dir) {
	__m128 v = _mm_set_ps(0.0f, dir.z, dir.y, dir.x);
	__m128 vx = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 vy = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vz = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));

	__m128 r = _mm_mul_ps(mt.rows[0], vx);
	r = _mm_add_ps(r, _mm_mul_ps(mt.rows[1], vy));
	r = _mm_add_ps(r, _mm_mul_ps(mt.rows[2], vz));

	float4 f4;
	f4.simd = r;
	return (float3){f4.x, f4.y, f4.z};
}

static inline float4 float4x4_transform_fast_float4(float4x4 mt, float4 v) {
	__m128 vx = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 vy = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vz = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vw = _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(3, 3, 3, 3));

	__m128 r = _mm_mul_ps(mt.rows[0], vx);
	r = _mm_add_ps(r, _mm_mul_ps(mt.rows[1], vy));
	r = _mm_add_ps(r, _mm_mul_ps(mt.rows[2], vz));
	r = _mm_add_ps(r, _mm_mul_ps(mt.rows[3], vw));
	return m128_to_float4(r);
}

static inline float3s float4x4_transform_fast_pt3s(float4x4 mt, float3s pt) {
	__m128 vz = _mm_shuffle_ps(pt.simd, pt.simd, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_shuffle_ps(pt.simd, pt.simd, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_shuffle_ps(pt.simd, pt.simd, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 r = _mm_add_ps(_mm_mul_ps(vz, mt.rows[2]), mt.rows[3]);
	r = _mm_add_ps(r, _mm_mul_ps(vy, mt.rows[1]));
	r = _mm_add_ps(r, _mm_mul_ps(vx, mt.rows[0]));
	return m128_to_float3s(r);
}

static inline float3s float4x4_transform_fast_dir3s(float4x4 mt, float3s dir) {
	__m128 vz = _mm_shuffle_ps(dir.simd, dir.simd, _MM_SHUFFLE(2, 2, 2, 2));
	__m128 vy = _mm_shuffle_ps(dir.simd, dir.simd, _MM_SHUFFLE(1, 1, 1, 1));
	__m128 vx = _mm_shuffle_ps(dir.simd, dir.simd, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 r = _mm_mul_ps(vz, mt.rows[2]);
	r = _mm_add_ps(r, _mm_mul_ps(vy, mt.rows[1]));
	r = _mm_add_ps(r, _mm_mul_ps(vx, mt.rows[0]));
	return m128_to_float3s(r);
}
#endif

static inline float4x4 float4x4_t(float3 translation) {
	float4x4 result;
	result.rows[0] = _mm_set_ps(translation.x, 0.0f, 0.0f, 1.0f);
	result.rows[1] = _mm_set_ps(translation.y, 0.0f, 1.0f, 0.0f);
	result.rows[2] = _mm_set_ps(translation.z, 1.0f, 0.0f, 0.0f);
	result.rows[3] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
	return result;
}

static inline float4x4 float4x4_r(float4 quat) {
	float4 q = float4_norm(quat);

	float xx = q.x * q.x;
	float yy = q.y * q.y;
	float zz = q.z * q.z;
	float xy = q.x * q.y;
	float xz = q.x * q.z;
	float yz = q.y * q.z;
	float wx = q.w * q.x;
	float wy = q.w * q.y;
	float wz = q.w * q.z;

	float4x4 result;
	result.rows[0] = _mm_set_ps(0.0f, 2.0f * (xz + wy), 2.0f * (xy - wz), 1.0f - 2.0f * (yy + zz));
	result.rows[1] = _mm_set_ps(0.0f, 2.0f * (yz - wx), 1.0f - 2.0f * (xx + zz), 2.0f * (xy + wz));
	result.rows[2] = _mm_set_ps(0.0f, 1.0f - 2.0f * (xx + yy), 2.0f * (yz + wx), 2.0f * (xz - wy));
	result.rows[3] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
	return result;
}

static inline float4x4 float4x4_s(float3 scale) {
	float4x4 result;
	result.rows[0] = _mm_set_ps(0.0f, 0.0f, 0.0f, scale.x);
	result.rows[1] = _mm_set_ps(0.0f, 0.0f, scale.y, 0.0f);
	result.rows[2] = _mm_set_ps(0.0f, scale.z, 0.0f, 0.0f);
	result.rows[3] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
	return result;
}

static inline float4x4 float4x4_trs(float3 translation, float4 rotation_quat, float3 scale) {
	float4 q = float4_norm(rotation_quat);

	float xx = q.x * q.x;
	float yy = q.y * q.y;
	float zz = q.z * q.z;
	float xy = q.x * q.y;
	float xz = q.x * q.z;
	float yz = q.y * q.z;
	float wx = q.w * q.x;
	float wy = q.w * q.y;
	float wz = q.w * q.z;

	float4x4 result;
	result.rows[0] = _mm_set_ps(translation.x,
	                            scale.x * (2.0f * (xz + wy)),
	                            scale.x * (2.0f * (xy - wz)),
	                            scale.x * (1.0f - 2.0f * (yy + zz)));
	result.rows[1] = _mm_set_ps(translation.y,
	                            scale.y * (2.0f * (yz - wx)),
	                            scale.y * (1.0f - 2.0f * (xx + zz)),
	                            scale.y * (2.0f * (xy + wz)));
	result.rows[2] = _mm_set_ps(translation.z,
	                            scale.z * (1.0f - 2.0f * (xx + yy)),
	                            scale.z * (2.0f * (yz + wx)),
	                            scale.z * (2.0f * (xz - wy)));
	result.rows[3] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
	return result;
}

static inline float4x4 float4x4_lookat(float3 eye, float3 target, float3 up) {
	float3 forward = float3_norm(float3_sub(target, eye));
	float3 right = float3_norm(float3_cross(forward, up));
	float3 actual_up = float3_cross(right, forward);

	float4x4 result;
	result.rows[0] = _mm_set_ps(-float3_dot(right, eye), right.z, right.y, right.x);
	result.rows[1] = _mm_set_ps(-float3_dot(actual_up, eye), actual_up.z, actual_up.y, actual_up.x);
	result.rows[2] = _mm_set_ps(float3_dot(forward, eye), -forward.z, -forward.y, -forward.x);
	result.rows[3] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
	return result;
}

static inline float4x4 float4x4_perspective(float fov_y, float aspect, float near_plane, float far_plane) {
	float tan_half_fov = tanf(fov_y * 0.5f);

	float4x4 result;
	result.rows[0] = _mm_set_ps(0.0f, 0.0f, 0.0f, 1.0f / (aspect * tan_half_fov));
	result.rows[1] = _mm_set_ps(0.0f, 0.0f, -1.0f / tan_half_fov, 0.0f);
	result.rows[2] = _mm_set_ps(-(far_plane * near_plane) / (far_plane - near_plane),
	                            far_plane / (near_plane - far_plane), 0.0f, 0.0f);
	result.rows[3] = _mm_set_ps(0.0f, -1.0f, 0.0f, 0.0f);
	return result;
}

static inline float4x4 float4x4_orthographic(float left, float right, float bottom, float top, float near_plane, float far_plane) {
	float4x4 result;
	result.rows[0] = _mm_set_ps(-(right + left) / (right - left), 0.0f, 0.0f, 2.0f / (right - left));
	result.rows[1] = _mm_set_ps((top + bottom) / (top - bottom), 0.0f, -2.0f / (top - bottom), 0.0f);
	result.rows[2] = _mm_set_ps(-near_plane / (far_plane - near_plane),
	                            -1.0f / (far_plane - near_plane), 0.0f, 0.0f);
	result.rows[3] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
	return result;
}

// ============================================================================
// Larger functions (kept at bottom for readability)
// ============================================================================

static inline float4x4 float4x4_invert(float4x4 m) {
	// Full SIMD matrix inversion based on DirectXMath XMMatrixInverse
	// Transpose first, compute 2x2 subdeterminants, then cofactors

	// Transpose matrix
	__m128 vTemp1 = _mm_shuffle_ps(m.rows[0], m.rows[1], _MM_SHUFFLE(1, 0, 1, 0));
	__m128 vTemp3 = _mm_shuffle_ps(m.rows[0], m.rows[1], _MM_SHUFFLE(3, 2, 3, 2));
	__m128 vTemp2 = _mm_shuffle_ps(m.rows[2], m.rows[3], _MM_SHUFFLE(1, 0, 1, 0));
	__m128 vTemp4 = _mm_shuffle_ps(m.rows[2], m.rows[3], _MM_SHUFFLE(3, 2, 3, 2));

	__m128 MT0 = _mm_shuffle_ps(vTemp1, vTemp2, _MM_SHUFFLE(2, 0, 2, 0));
	__m128 MT1 = _mm_shuffle_ps(vTemp1, vTemp2, _MM_SHUFFLE(3, 1, 3, 1));
	__m128 MT2 = _mm_shuffle_ps(vTemp3, vTemp4, _MM_SHUFFLE(2, 0, 2, 0));
	__m128 MT3 = _mm_shuffle_ps(vTemp3, vTemp4, _MM_SHUFFLE(3, 1, 3, 1));

	// Compute 2x2 subdeterminants
	__m128 V00 = _mm_shuffle_ps(MT2, MT2, _MM_SHUFFLE(1, 1, 0, 0));
	__m128 V10 = _mm_shuffle_ps(MT3, MT3, _MM_SHUFFLE(3, 2, 3, 2));
	__m128 V01 = _mm_shuffle_ps(MT0, MT0, _MM_SHUFFLE(1, 1, 0, 0));
	__m128 V11 = _mm_shuffle_ps(MT1, MT1, _MM_SHUFFLE(3, 2, 3, 2));
	__m128 V02 = _mm_shuffle_ps(MT2, MT0, _MM_SHUFFLE(2, 0, 2, 0));
	__m128 V12 = _mm_shuffle_ps(MT3, MT1, _MM_SHUFFLE(3, 1, 3, 1));

	__m128 D0 = _mm_mul_ps(V00, V10);
	__m128 D1 = _mm_mul_ps(V01, V11);
	__m128 D2 = _mm_mul_ps(V02, V12);

	V00 = _mm_shuffle_ps(MT2, MT2, _MM_SHUFFLE(3, 2, 3, 2));
	V10 = _mm_shuffle_ps(MT3, MT3, _MM_SHUFFLE(1, 1, 0, 0));
	V01 = _mm_shuffle_ps(MT0, MT0, _MM_SHUFFLE(3, 2, 3, 2));
	V11 = _mm_shuffle_ps(MT1, MT1, _MM_SHUFFLE(1, 1, 0, 0));
	V02 = _mm_shuffle_ps(MT2, MT0, _MM_SHUFFLE(3, 1, 3, 1));
	V12 = _mm_shuffle_ps(MT3, MT1, _MM_SHUFFLE(2, 0, 2, 0));

#if defined(__FMA__)
	D0 = _mm_fnmadd_ps(V00, V10, D0);
	D1 = _mm_fnmadd_ps(V01, V11, D1);
	D2 = _mm_fnmadd_ps(V02, V12, D2);
#else
	D0 = _mm_sub_ps(D0, _mm_mul_ps(V00, V10));
	D1 = _mm_sub_ps(D1, _mm_mul_ps(V01, V11));
	D2 = _mm_sub_ps(D2, _mm_mul_ps(V02, V12));
#endif

	// Build cofactors
	V11 = _mm_shuffle_ps(D0, D2, _MM_SHUFFLE(1, 1, 3, 1));
	V00 = _mm_shuffle_ps(MT1, MT1, _MM_SHUFFLE(1, 0, 2, 1));
	V10 = _mm_shuffle_ps(V11, D0, _MM_SHUFFLE(0, 3, 0, 2));
	V01 = _mm_shuffle_ps(MT0, MT0, _MM_SHUFFLE(0, 1, 0, 2));
	V11 = _mm_shuffle_ps(V11, D0, _MM_SHUFFLE(2, 1, 2, 1));
	__m128 V13 = _mm_shuffle_ps(D1, D2, _MM_SHUFFLE(3, 3, 3, 1));
	V02 = _mm_shuffle_ps(MT3, MT3, _MM_SHUFFLE(1, 0, 2, 1));
	V12 = _mm_shuffle_ps(V13, D1, _MM_SHUFFLE(0, 3, 0, 2));
	__m128 V03 = _mm_shuffle_ps(MT2, MT2, _MM_SHUFFLE(0, 1, 0, 2));
	V13 = _mm_shuffle_ps(V13, D1, _MM_SHUFFLE(2, 1, 2, 1));

	__m128 C0 = _mm_mul_ps(V00, V10);
	__m128 C2 = _mm_mul_ps(V01, V11);
	__m128 C4 = _mm_mul_ps(V02, V12);
	__m128 C6 = _mm_mul_ps(V03, V13);

	V11 = _mm_shuffle_ps(D0, D2, _MM_SHUFFLE(0, 0, 1, 0));
	V00 = _mm_shuffle_ps(MT1, MT1, _MM_SHUFFLE(2, 1, 3, 2));
	V10 = _mm_shuffle_ps(D0, V11, _MM_SHUFFLE(2, 1, 0, 3));
	V01 = _mm_shuffle_ps(MT0, MT0, _MM_SHUFFLE(1, 3, 2, 3));
	V11 = _mm_shuffle_ps(D0, V11, _MM_SHUFFLE(0, 2, 1, 2));
	V13 = _mm_shuffle_ps(D1, D2, _MM_SHUFFLE(2, 2, 1, 0));
	V02 = _mm_shuffle_ps(MT3, MT3, _MM_SHUFFLE(2, 1, 3, 2));
	V12 = _mm_shuffle_ps(D1, V13, _MM_SHUFFLE(2, 1, 0, 3));
	V03 = _mm_shuffle_ps(MT2, MT2, _MM_SHUFFLE(1, 3, 2, 3));
	V13 = _mm_shuffle_ps(D1, V13, _MM_SHUFFLE(0, 2, 1, 2));

#if defined(__FMA__)
	C0 = _mm_fnmadd_ps(V00, V10, C0);
	C2 = _mm_fnmadd_ps(V01, V11, C2);
	C4 = _mm_fnmadd_ps(V02, V12, C4);
	C6 = _mm_fnmadd_ps(V03, V13, C6);
#else
	C0 = _mm_sub_ps(C0, _mm_mul_ps(V00, V10));
	C2 = _mm_sub_ps(C2, _mm_mul_ps(V01, V11));
	C4 = _mm_sub_ps(C4, _mm_mul_ps(V02, V12));
	C6 = _mm_sub_ps(C6, _mm_mul_ps(V03, V13));
#endif

	V00 = _mm_shuffle_ps(MT1, MT1, _MM_SHUFFLE(0, 3, 0, 3));
	V10 = _mm_shuffle_ps(D0, D2, _MM_SHUFFLE(1, 0, 2, 2));
	V10 = _mm_shuffle_ps(V10, V10, _MM_SHUFFLE(0, 2, 3, 0));
	V01 = _mm_shuffle_ps(MT0, MT0, _MM_SHUFFLE(2, 0, 3, 1));
	V11 = _mm_shuffle_ps(D0, D2, _MM_SHUFFLE(1, 0, 3, 0));
	V11 = _mm_shuffle_ps(V11, V11, _MM_SHUFFLE(2, 1, 0, 3));
	V02 = _mm_shuffle_ps(MT3, MT3, _MM_SHUFFLE(0, 3, 0, 3));
	V12 = _mm_shuffle_ps(D1, D2, _MM_SHUFFLE(3, 2, 2, 2));
	V12 = _mm_shuffle_ps(V12, V12, _MM_SHUFFLE(0, 2, 3, 0));
	V03 = _mm_shuffle_ps(MT2, MT2, _MM_SHUFFLE(2, 0, 3, 1));
	V13 = _mm_shuffle_ps(D1, D2, _MM_SHUFFLE(3, 2, 3, 0));
	V13 = _mm_shuffle_ps(V13, V13, _MM_SHUFFLE(2, 1, 0, 3));

	V00 = _mm_mul_ps(V00, V10);
	V01 = _mm_mul_ps(V01, V11);
	V02 = _mm_mul_ps(V02, V12);
	V03 = _mm_mul_ps(V03, V13);
	__m128 C1 = _mm_sub_ps(C0, V00);
	C0 = _mm_add_ps(C0, V00);
	__m128 C3 = _mm_add_ps(C2, V01);
	C2 = _mm_sub_ps(C2, V01);
	__m128 C5 = _mm_sub_ps(C4, V02);
	C4 = _mm_add_ps(C4, V02);
	__m128 C7 = _mm_add_ps(C6, V03);
	C6 = _mm_sub_ps(C6, V03);

	C0 = _mm_shuffle_ps(C0, C1, _MM_SHUFFLE(3, 1, 2, 0));
	C2 = _mm_shuffle_ps(C2, C3, _MM_SHUFFLE(3, 1, 2, 0));
	C4 = _mm_shuffle_ps(C4, C5, _MM_SHUFFLE(3, 1, 2, 0));
	C6 = _mm_shuffle_ps(C6, C7, _MM_SHUFFLE(3, 1, 2, 0));
	C0 = _mm_shuffle_ps(C0, C0, _MM_SHUFFLE(3, 1, 2, 0));
	C2 = _mm_shuffle_ps(C2, C2, _MM_SHUFFLE(3, 1, 2, 0));
	C4 = _mm_shuffle_ps(C4, C4, _MM_SHUFFLE(3, 1, 2, 0));
	C6 = _mm_shuffle_ps(C6, C6, _MM_SHUFFLE(3, 1, 2, 0));

	// Compute determinant via dot product
	__m128 det = _mm_mul_ps(C0, MT0);
	det = _mm_add_ps(det, _mm_shuffle_ps(det, det, _MM_SHUFFLE(2, 3, 0, 1)));
	det = _mm_add_ss(det, _mm_shuffle_ps(det, det, _MM_SHUFFLE(1, 0, 3, 2)));

	// Check for zero determinant
	float det_f;
	_mm_store_ss(&det_f, det);
	if (det_f == 0.0f) {
		return float4x4_identity();
	}

	// Compute reciprocal and multiply
	__m128 rcp = _mm_div_ss(_mm_set_ss(1.0f), det);
	rcp = _mm_shuffle_ps(rcp, rcp, _MM_SHUFFLE(0, 0, 0, 0));

	float4x4 result;
	result.rows[0] = _mm_mul_ps(C0, rcp);
	result.rows[1] = _mm_mul_ps(C2, rcp);
	result.rows[2] = _mm_mul_ps(C4, rcp);
	result.rows[3] = _mm_mul_ps(C6, rcp);
	return result;
}

#ifdef __cplusplus
}
#endif


#endif // FLOAT_MATH_USE_SSE

// ============================================================================
// NEON implementation
// ============================================================================
#if defined(FLOAT_MATH_USE_NEON)


#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <arm_neon.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Vector types (NEON optimized)
// ============================================================================

typedef struct { float x, y; } float2;

// float3: 12 bytes, matches user expectation for size
typedef struct { float x, y, z; } float3;

// float3s: 16 bytes, SIMD-aligned for NEON operations
typedef union {
	struct { float x, y, z, _pad; };
	float v[4];
	float32x4_t simd;
} __attribute__((aligned(16))) float3s;

// float4: 16 bytes, SIMD-aligned
typedef union {
	struct { float x, y, z, w; };
	struct { float r, g, b, a; };
	float v[4];
	float32x4_t simd;
} __attribute__((aligned(16))) float4;

// Matrix type (row-major for direct shader compatibility)
typedef union {
	float m[16]; // Row-major: [m00, m01, m02, m03, m10, m11, ...]
	float32x4_t rows[4];
} __attribute__((aligned(16))) float4x4;

// ============================================================================
// Conversions between float3 and float3s
// ============================================================================

static inline float3 float3s_to_float3(float3s v) {
	return (float3){v.x, v.y, v.z};
}

static inline float3s float3_to_float3s(float3 v) {
	float3s result;
	float tmp[4] = {v.x, v.y, v.z, 0.0f};
	result.simd = vld1q_f32(tmp);
	return result;
}

// ============================================================================
// NEON Helper functions
// ============================================================================

static inline float3s neon_to_float3s(float32x4_t v) {
	float3s result;
	result.simd = v;
	return result;
}

static inline float4 neon_to_float4(float32x4_t v) {
	float4 result;
	result.simd = v;
	return result;
}

// Horizontal add for dot product
static inline float neon_horizontal_add(float32x4_t v) {
	float32x2_t sum = vadd_f32(vget_low_f32(v), vget_high_f32(v));
	return vget_lane_f32(vpadd_f32(sum, sum), 0);
}

// ============================================================================
// Float2 operations (scalar - NEON not beneficial for 2 floats)
// ============================================================================

static inline float2 float2_add  (float2 a, float2 b) { return (float2){a.x + b.x, a.y + b.y}; }
static inline float2 float2_add_s(float2 a, float  s) { return (float2){a.x + s, a.y + s}; }
static inline float2 float2_sub  (float2 a, float2 b) { return (float2){a.x - b.x, a.y - b.y}; }
static inline float2 float2_sub_s(float2 a, float  s) { return (float2){a.x - s, a.y - s}; }
static inline float2 float2_mul  (float2 a, float2 b) { return (float2){a.x * b.x, a.y * b.y}; }
static inline float2 float2_mul_s(float2 a, float  s) { return (float2){a.x * s, a.y * s}; }
static inline float2 float2_div  (float2 a, float2 b) { return (float2){a.x / b.x, a.y / b.y}; }
static inline float2 float2_div_s(float2 a, float  s) { return (float2){a.x / s, a.y / s}; }
static inline float  float2_dot  (float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
static inline float  float2_mag2 (float2 v)           { return float2_dot(v, v); }
static inline float  float2_dist2(float2 a, float2 b) { return float2_mag2(float2_sub(a, b)); }

static inline float2 float2_frac(float2 v) {
	return (float2){v.x - floorf(v.x), v.y - floorf(v.y)};
}

static inline float2 float2_floor(float2 v) {
	return (float2){floorf(v.x), floorf(v.y)};
}

static inline float2 float2_ceil(float2 v) {
	return (float2){ceilf(v.x), ceilf(v.y)};
}

static inline float2 float2_abs(float2 v) {
	return (float2){fabsf(v.x), fabsf(v.y)};
}

static inline float2 float2_min(float2 a, float2 b) {
	return (float2){fminf(a.x, b.x), fminf(a.y, b.y)};
}

static inline float2 float2_max(float2 a, float2 b) {
	return (float2){fmaxf(a.x, b.x), fmaxf(a.y, b.y)};
}

static inline float float2_mag(float2 v) {
	return sqrtf(float2_mag2(v));
}

static inline float float2_dist(float2 a, float2 b) {
	return sqrtf(float2_dist2(a, b));
}

static inline float2 float2_norm(float2 v) {
	float mag = float2_mag(v);
	if (mag == 0.0f) {
		return (float2){0.0f, 0.0f};
	}
	return float2_div_s(v, mag);
}

// ============================================================================
// Float3 operations (scalar - 12 bytes, no padding)
// ============================================================================

static inline float3 float3_add  (float3 a, float3 b) { return (float3){a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline float3 float3_add_s(float3 a, float  s) { return (float3){a.x + s, a.y + s, a.z + s}; }
static inline float3 float3_sub  (float3 a, float3 b) { return (float3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline float3 float3_sub_s(float3 a, float  s) { return (float3){a.x - s, a.y - s, a.z - s}; }
static inline float3 float3_mul  (float3 a, float3 b) { return (float3){a.x * b.x, a.y * b.y, a.z * b.z}; }
static inline float3 float3_mul_s(float3 a, float  s) { return (float3){a.x * s, a.y * s, a.z * s}; }
static inline float3 float3_div  (float3 a, float3 b) { return (float3){a.x / b.x, a.y / b.y, a.z / b.z}; }
static inline float3 float3_div_s(float3 a, float  s) { return (float3){a.x / s, a.y / s, a.z / s}; }
static inline float  float3_dot  (float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float  float3_mag2 (float3 v)           { return float3_dot(v, v); }
static inline float  float3_dist2(float3 a, float3 b) { return float3_mag2(float3_sub(a, b)); }
static inline float3 float3_cross(float3 a, float3 b) {
	return (float3){
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
}

static inline float3 float3_frac(float3 v) {
	return (float3){v.x - floorf(v.x), v.y - floorf(v.y), v.z - floorf(v.z)};
}

static inline float3 float3_floor(float3 v) {
	return (float3){floorf(v.x), floorf(v.y), floorf(v.z)};
}

static inline float3 float3_ceil(float3 v) {
	return (float3){ceilf(v.x), ceilf(v.y), ceilf(v.z)};
}

static inline float3 float3_abs(float3 v) {
	return (float3){fabsf(v.x), fabsf(v.y), fabsf(v.z)};
}

static inline float3 float3_min(float3 a, float3 b) {
	return (float3){fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z)};
}

static inline float3 float3_max(float3 a, float3 b) {
	return (float3){fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z)};
}

static inline float float3_mag(float3 v) {
	return sqrtf(float3_mag2(v));
}

static inline float float3_dist(float3 a, float3 b) {
	return sqrtf(float3_dist2(a, b));
}

static inline float3 float3_norm(float3 v) {
	float mag = float3_mag(v);
	if (mag == 0.0f) {
		return (float3){0.0f, 0.0f, 0.0f};
	}
	return float3_div_s(v, mag);
}

// ============================================================================
// Float3s operations (NEON accelerated - 16 bytes, SIMD aligned)
// ============================================================================

static inline float3s float3s_add(float3s a, float3s b) {
	return neon_to_float3s(vaddq_f32(a.simd, b.simd));
}

static inline float3s float3s_add_s(float3s a, float s) {
	return neon_to_float3s(vaddq_f32(a.simd, vdupq_n_f32(s)));
}

static inline float3s float3s_sub(float3s a, float3s b) {
	return neon_to_float3s(vsubq_f32(a.simd, b.simd));
}

static inline float3s float3s_sub_s(float3s a, float s) {
	return neon_to_float3s(vsubq_f32(a.simd, vdupq_n_f32(s)));
}

static inline float3s float3s_mul(float3s a, float3s b) {
	return neon_to_float3s(vmulq_f32(a.simd, b.simd));
}

static inline float3s float3s_mul_s(float3s a, float s) {
	return neon_to_float3s(vmulq_n_f32(a.simd, s));
}

static inline float3s float3s_div(float3s a, float3s b) {
	return neon_to_float3s(vdivq_f32(a.simd, b.simd));
}

static inline float3s float3s_div_s(float3s a, float s) {
	return neon_to_float3s(vdivq_f32(a.simd, vdupq_n_f32(s)));
}

static inline float float3s_dot(float3s a, float3s b) {
	// Multiply and mask out w component before summing
	float32x4_t prod = vmulq_f32(a.simd, b.simd);
	// Zero out the w component (index 3)
	prod = vsetq_lane_f32(0.0f, prod, 3);
	return neon_horizontal_add(prod);
}

static inline float float3s_mag2(float3s v) {
	return float3s_dot(v, v);
}

static inline float float3s_dist2(float3s a, float3s b) {
	return float3s_mag2(float3s_sub(a, b));
}

static inline float3s float3s_cross(float3s a, float3s b) {
	// Cross product: a.yzx * b.zxy - a.zxy * b.yzx
	// NEON shuffle using vextq_f32 and combining elements
	float32x4_t a_yzx = vextq_f32(a.simd, a.simd, 1);  // rotate left by 1: y,z,w,x
	a_yzx = vsetq_lane_f32(vgetq_lane_f32(a.simd, 0), a_yzx, 2);  // y,z,x,x

	float32x4_t b_zxy = vextq_f32(b.simd, b.simd, 2);  // rotate left by 2: z,w,x,y
	b_zxy = vsetq_lane_f32(vgetq_lane_f32(b.simd, 1), b_zxy, 2);  // z,x,y,y

	float32x4_t a_zxy = vextq_f32(a.simd, a.simd, 2);  // rotate left by 2: z,w,x,y
	a_zxy = vsetq_lane_f32(vgetq_lane_f32(a.simd, 1), a_zxy, 2);  // z,x,y,y

	float32x4_t b_yzx = vextq_f32(b.simd, b.simd, 1);  // rotate left by 1: y,z,w,x
	b_yzx = vsetq_lane_f32(vgetq_lane_f32(b.simd, 0), b_yzx, 2);  // y,z,x,x

	float32x4_t result = vmlsq_f32(vmulq_f32(a_yzx, b_zxy), a_zxy, b_yzx);
	result = vsetq_lane_f32(0.0f, result, 3);  // Zero pad
	return neon_to_float3s(result);
}

static inline float3s float3s_frac(float3s v) {
	float32x4_t floor_v = vrndmq_f32(v.simd);
	return neon_to_float3s(vsubq_f32(v.simd, floor_v));
}

static inline float3s float3s_floor(float3s v) {
	return neon_to_float3s(vrndmq_f32(v.simd));
}

static inline float3s float3s_ceil(float3s v) {
	return neon_to_float3s(vrndpq_f32(v.simd));
}

static inline float3s float3s_abs(float3s v) {
	return neon_to_float3s(vabsq_f32(v.simd));
}

static inline float3s float3s_min(float3s a, float3s b) {
	return neon_to_float3s(vminq_f32(a.simd, b.simd));
}

static inline float3s float3s_max(float3s a, float3s b) {
	return neon_to_float3s(vmaxq_f32(a.simd, b.simd));
}

static inline float float3s_mag(float3s v) {
	return sqrtf(float3s_mag2(v));
}

static inline float float3s_dist(float3s a, float3s b) {
	return sqrtf(float3s_dist2(a, b));
}

static inline float3s float3s_norm(float3s v) {
	float mag2 = float3s_mag2(v);
	if (mag2 == 0.0f) {
		float3s zero = {0};
		return zero;
	}
	// Use rsqrt estimate with Newton-Raphson refinement
	float32x4_t mag2_v = vdupq_n_f32(mag2);
	float32x4_t inv_mag = vrsqrteq_f32(mag2_v);
	// Newton-Raphson: x' = x * (3 - d * x * x) / 2
	inv_mag = vmulq_f32(inv_mag, vrsqrtsq_f32(vmulq_f32(mag2_v, inv_mag), inv_mag));
	inv_mag = vmulq_f32(inv_mag, vrsqrtsq_f32(vmulq_f32(mag2_v, inv_mag), inv_mag));
	return neon_to_float3s(vmulq_f32(v.simd, inv_mag));
}

// ============================================================================
// Float4 operations (NEON accelerated)
// ============================================================================

static inline float4 float4_add(float4 a, float4 b) {
	return neon_to_float4(vaddq_f32(a.simd, b.simd));
}

static inline float4 float4_add_s(float4 a, float s) {
	return neon_to_float4(vaddq_f32(a.simd, vdupq_n_f32(s)));
}

static inline float4 float4_sub(float4 a, float4 b) {
	return neon_to_float4(vsubq_f32(a.simd, b.simd));
}

static inline float4 float4_sub_s(float4 a, float s) {
	return neon_to_float4(vsubq_f32(a.simd, vdupq_n_f32(s)));
}

static inline float4 float4_mul(float4 a, float4 b) {
	return neon_to_float4(vmulq_f32(a.simd, b.simd));
}

static inline float4 float4_mul_s(float4 a, float s) {
	return neon_to_float4(vmulq_n_f32(a.simd, s));
}

static inline float4 float4_div(float4 a, float4 b) {
	return neon_to_float4(vdivq_f32(a.simd, b.simd));
}

static inline float4 float4_div_s(float4 a, float s) {
	return neon_to_float4(vdivq_f32(a.simd, vdupq_n_f32(s)));
}

static inline float float4_dot(float4 a, float4 b) {
	float32x4_t prod = vmulq_f32(a.simd, b.simd);
	return neon_horizontal_add(prod);
}

static inline float float4_mag2(float4 v) {
	return float4_dot(v, v);
}

static inline float float4_dist2(float4 a, float4 b) {
	return float4_mag2(float4_sub(a, b));
}

static inline float4 float4_frac(float4 v) {
	float32x4_t floor_v = vrndmq_f32(v.simd);
	return neon_to_float4(vsubq_f32(v.simd, floor_v));
}

static inline float4 float4_floor(float4 v) {
	return neon_to_float4(vrndmq_f32(v.simd));
}

static inline float4 float4_ceil(float4 v) {
	return neon_to_float4(vrndpq_f32(v.simd));
}

static inline float4 float4_abs(float4 v) {
	return neon_to_float4(vabsq_f32(v.simd));
}

static inline float4 float4_min(float4 a, float4 b) {
	return neon_to_float4(vminq_f32(a.simd, b.simd));
}

static inline float4 float4_max(float4 a, float4 b) {
	return neon_to_float4(vmaxq_f32(a.simd, b.simd));
}

static inline float float4_mag(float4 v) {
	return sqrtf(float4_mag2(v));
}

static inline float float4_dist(float4 a, float4 b) {
	return sqrtf(float4_dist2(a, b));
}

static inline float4 float4_norm(float4 v) {
	float mag2 = float4_mag2(v);
	if (mag2 == 0.0f) {
		float4 zero = {0};
		return zero;
	}
	// Use rsqrt estimate with Newton-Raphson refinement
	float32x4_t mag2_v = vdupq_n_f32(mag2);
	float32x4_t inv_mag = vrsqrteq_f32(mag2_v);
	// Newton-Raphson iterations
	inv_mag = vmulq_f32(inv_mag, vrsqrtsq_f32(vmulq_f32(mag2_v, inv_mag), inv_mag));
	inv_mag = vmulq_f32(inv_mag, vrsqrtsq_f32(vmulq_f32(mag2_v, inv_mag), inv_mag));
	return neon_to_float4(vmulq_f32(v.simd, inv_mag));
}

// ============================================================================
// Quaternion operations (using float4)
// ============================================================================

// Conjugate (inverse for unit quaternions)
static inline float4 float4_quat_conjugate(float4 q) {
	// Negate xyz, keep w
	float32x4_t sign = {-1.0f, -1.0f, -1.0f, 1.0f};
	return neon_to_float4(vmulq_f32(q.simd, sign));
}

static inline float4 float4_quat_from_euler(float3 euler_xyz) {
	// Half angles
	float cx = cosf(euler_xyz.x * 0.5f);
	float sx = sinf(euler_xyz.x * 0.5f);
	float cy = cosf(euler_xyz.y * 0.5f);
	float sy = sinf(euler_xyz.y * 0.5f);
	float cz = cosf(euler_xyz.z * 0.5f);
	float sz = sinf(euler_xyz.z * 0.5f);

	// XYZ order
	float4 result;
	result.x = sx * cy * cz - cx * sy * sz;
	result.y = cx * sy * cz + sx * cy * sz;
	result.z = cx * cy * sz - sx * sy * cz;
	result.w = cx * cy * cz + sx * sy * sz;
	return result;
}

static inline float4 float4_quat_from_axis_angle(float3 axis, float angle) {
	float half_angle = angle * 0.5f;
	float s = sinf(half_angle);
	float4 result;
	result.x = axis.x * s;
	result.y = axis.y * s;
	result.z = axis.z * s;
	result.w = cosf(half_angle);
	return result;
}

static inline float4 float4_quat_mul(float4 a, float4 b) {
	// Standard quaternion multiplication formula
	float4 result;
	result.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	result.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	result.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
	result.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	return result;
}

static inline float3 float4_quat_rotate(float4 q, float3 v) {
	// v' = q * v * q^-1
	float3 qv = {q.x, q.y, q.z};
	float3 uv = float3_cross(qv, v);
	float3 uuv = float3_cross(qv, uv);
	uv = float3_mul_s(uv, 2.0f * q.w);
	uuv = float3_mul_s(uuv, 2.0f);
	return float3_add(float3_add(v, uv), uuv);
}

// ============================================================================
// Matrix operations
// ============================================================================

static inline float4x4 float4x4_identity(void) {
	float4x4 result;
	result.rows[0] = (float32x4_t){1.0f, 0.0f, 0.0f, 0.0f};
	result.rows[1] = (float32x4_t){0.0f, 1.0f, 0.0f, 0.0f};
	result.rows[2] = (float32x4_t){0.0f, 0.0f, 1.0f, 0.0f};
	result.rows[3] = (float32x4_t){0.0f, 0.0f, 0.0f, 1.0f};
	return result;
}

static inline float4x4 float4x4_mul(float4x4 a, float4x4 b) {
	float4x4 result;

	for (int i = 0; i < 4; i++) {
		float32x4_t row = vmulq_laneq_f32(b.rows[0], a.rows[i], 0);
		row = vfmaq_laneq_f32(row, b.rows[1], a.rows[i], 1);
		row = vfmaq_laneq_f32(row, b.rows[2], a.rows[i], 2);
		row = vfmaq_laneq_f32(row, b.rows[3], a.rows[i], 3);
		result.rows[i] = row;
	}

	return result;
}

static inline float4x4 float4x4_transpose(float4x4 m) {
	float4x4 result;

	// Transpose using vtrn and vzip operations
	float32x4x2_t t01 = vtrnq_f32(m.rows[0], m.rows[1]);
	float32x4x2_t t23 = vtrnq_f32(m.rows[2], m.rows[3]);

	result.rows[0] = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
	result.rows[1] = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
	result.rows[2] = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
	result.rows[3] = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t23.val[1]));

	return result;
}

static inline float3 float4x4_transform_pt(float4x4 m, float3 pt) {
	float4x4 mt = float4x4_transpose(m);

	float32x4_t v = {pt.x, pt.y, pt.z, 1.0f};

	float32x4_t r = vmulq_laneq_f32(mt.rows[0], v, 0);
	r = vfmaq_laneq_f32(r, mt.rows[1], v, 1);
	r = vfmaq_laneq_f32(r, mt.rows[2], v, 2);
	r = vfmaq_laneq_f32(r, mt.rows[3], v, 3);

	return (float3){vgetq_lane_f32(r, 0), vgetq_lane_f32(r, 1), vgetq_lane_f32(r, 2)};
}

static inline float3 float4x4_transform_dir(float4x4 m, float3 dir) {
	float4x4 mt = float4x4_transpose(m);

	float32x4_t v = {dir.x, dir.y, dir.z, 0.0f};

	float32x4_t r = vmulq_laneq_f32(mt.rows[0], v, 0);
	r = vfmaq_laneq_f32(r, mt.rows[1], v, 1);
	r = vfmaq_laneq_f32(r, mt.rows[2], v, 2);

	return (float3){vgetq_lane_f32(r, 0), vgetq_lane_f32(r, 1), vgetq_lane_f32(r, 2)};
}

static inline float4 float4x4_transform_float4(float4x4 m, float4 v) {
	float4x4 mt = float4x4_transpose(m);

	float32x4_t r = vmulq_laneq_f32(mt.rows[0], v.simd, 0);
	r = vfmaq_laneq_f32(r, mt.rows[1], v.simd, 1);
	r = vfmaq_laneq_f32(r, mt.rows[2], v.simd, 2);
	r = vfmaq_laneq_f32(r, mt.rows[3], v.simd, 3);

	return neon_to_float4(r);
}

// Fast transforms using pre-transposed matrix (columns stored in rows[0..3])
static inline float3 float4x4_transform_fast_pt(float4x4 mt, float3 pt) {
	float32x4_t v = {pt.x, pt.y, pt.z, 0.0f};

	float32x4_t r = vfmaq_laneq_f32(mt.rows[3], mt.rows[0], v, 0);
	r = vfmaq_laneq_f32(r, mt.rows[1], v, 1);
	r = vfmaq_laneq_f32(r, mt.rows[2], v, 2);

	return (float3){vgetq_lane_f32(r, 0), vgetq_lane_f32(r, 1), vgetq_lane_f32(r, 2)};
}

static inline float3 float4x4_transform_fast_dir(float4x4 mt, float3 dir) {
	float32x4_t v = {dir.x, dir.y, dir.z, 0.0f};

	float32x4_t r = vmulq_laneq_f32(mt.rows[0], v, 0);
	r = vfmaq_laneq_f32(r, mt.rows[1], v, 1);
	r = vfmaq_laneq_f32(r, mt.rows[2], v, 2);

	return (float3){vgetq_lane_f32(r, 0), vgetq_lane_f32(r, 1), vgetq_lane_f32(r, 2)};
}

static inline float4 float4x4_transform_fast_float4(float4x4 mt, float4 v) {
	float32x4_t r = vmulq_laneq_f32(mt.rows[0], v.simd, 0);
	r = vfmaq_laneq_f32(r, mt.rows[1], v.simd, 1);
	r = vfmaq_laneq_f32(r, mt.rows[2], v.simd, 2);
	r = vfmaq_laneq_f32(r, mt.rows[3], v.simd, 3);
	return neon_to_float4(r);
}

static inline float3s float4x4_transform_fast_pt3s(float4x4 mt, float3s pt) {
	float32x4_t r = vfmaq_laneq_f32(mt.rows[3], mt.rows[0], pt.simd, 0);
	r = vfmaq_laneq_f32(r, mt.rows[1], pt.simd, 1);
	r = vfmaq_laneq_f32(r, mt.rows[2], pt.simd, 2);
	return neon_to_float3s(r);
}

static inline float3s float4x4_transform_fast_dir3s(float4x4 mt, float3s dir) {
	float32x4_t r = vmulq_laneq_f32(mt.rows[0], dir.simd, 0);
	r = vfmaq_laneq_f32(r, mt.rows[1], dir.simd, 1);
	r = vfmaq_laneq_f32(r, mt.rows[2], dir.simd, 2);
	return neon_to_float3s(r);
}

static inline float4x4 float4x4_t(float3 translation) {
	float4x4 result;
	result.rows[0] = (float32x4_t){1.0f, 0.0f, 0.0f, translation.x};
	result.rows[1] = (float32x4_t){0.0f, 1.0f, 0.0f, translation.y};
	result.rows[2] = (float32x4_t){0.0f, 0.0f, 1.0f, translation.z};
	result.rows[3] = (float32x4_t){0.0f, 0.0f, 0.0f, 1.0f};
	return result;
}

static inline float4x4 float4x4_r(float4 quat) {
	float4 q = float4_norm(quat);

	float xx = q.x * q.x;
	float yy = q.y * q.y;
	float zz = q.z * q.z;
	float xy = q.x * q.y;
	float xz = q.x * q.z;
	float yz = q.y * q.z;
	float wx = q.w * q.x;
	float wy = q.w * q.y;
	float wz = q.w * q.z;

	float4x4 result;
	result.rows[0] = (float32x4_t){1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy), 0.0f};
	result.rows[1] = (float32x4_t){2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx), 0.0f};
	result.rows[2] = (float32x4_t){2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy), 0.0f};
	result.rows[3] = (float32x4_t){0.0f, 0.0f, 0.0f, 1.0f};
	return result;
}

static inline float4x4 float4x4_s(float3 scale) {
	float4x4 result;
	result.rows[0] = (float32x4_t){scale.x, 0.0f, 0.0f, 0.0f};
	result.rows[1] = (float32x4_t){0.0f, scale.y, 0.0f, 0.0f};
	result.rows[2] = (float32x4_t){0.0f, 0.0f, scale.z, 0.0f};
	result.rows[3] = (float32x4_t){0.0f, 0.0f, 0.0f, 1.0f};
	return result;
}

static inline float4x4 float4x4_trs(float3 translation, float4 rotation_quat, float3 scale) {
	float4 q = float4_norm(rotation_quat);

	float xx = q.x * q.x;
	float yy = q.y * q.y;
	float zz = q.z * q.z;
	float xy = q.x * q.y;
	float xz = q.x * q.z;
	float yz = q.y * q.z;
	float wx = q.w * q.x;
	float wy = q.w * q.y;
	float wz = q.w * q.z;

	float4x4 result;
	result.rows[0] = (float32x4_t){
		scale.x * (1.0f - 2.0f * (yy + zz)),
		scale.x * (2.0f * (xy - wz)),
		scale.x * (2.0f * (xz + wy)),
		translation.x
	};
	result.rows[1] = (float32x4_t){
		scale.y * (2.0f * (xy + wz)),
		scale.y * (1.0f - 2.0f * (xx + zz)),
		scale.y * (2.0f * (yz - wx)),
		translation.y
	};
	result.rows[2] = (float32x4_t){
		scale.z * (2.0f * (xz - wy)),
		scale.z * (2.0f * (yz + wx)),
		scale.z * (1.0f - 2.0f * (xx + yy)),
		translation.z
	};
	result.rows[3] = (float32x4_t){0.0f, 0.0f, 0.0f, 1.0f};
	return result;
}

static inline float4x4 float4x4_lookat(float3 eye, float3 target, float3 up) {
	float3 forward = float3_norm(float3_sub(target, eye));
	float3 right = float3_norm(float3_cross(forward, up));
	float3 actual_up = float3_cross(right, forward);

	float4x4 result;
	result.rows[0] = (float32x4_t){right.x, right.y, right.z, -float3_dot(right, eye)};
	result.rows[1] = (float32x4_t){actual_up.x, actual_up.y, actual_up.z, -float3_dot(actual_up, eye)};
	result.rows[2] = (float32x4_t){-forward.x, -forward.y, -forward.z, float3_dot(forward, eye)};
	result.rows[3] = (float32x4_t){0.0f, 0.0f, 0.0f, 1.0f};
	return result;
}

static inline float4x4 float4x4_perspective(float fov_y, float aspect, float near_plane, float far_plane) {
	float tan_half_fov = tanf(fov_y * 0.5f);

	float4x4 result;
	result.rows[0] = (float32x4_t){1.0f / (aspect * tan_half_fov), 0.0f, 0.0f, 0.0f};
	result.rows[1] = (float32x4_t){0.0f, -1.0f / tan_half_fov, 0.0f, 0.0f};
	result.rows[2] = (float32x4_t){0.0f, 0.0f, far_plane / (near_plane - far_plane), -(far_plane * near_plane) / (far_plane - near_plane)};
	result.rows[3] = (float32x4_t){0.0f, 0.0f, -1.0f, 0.0f};
	return result;
}

static inline float4x4 float4x4_orthographic(float left, float right, float bottom, float top, float near_plane, float far_plane) {
	float4x4 result;
	result.rows[0] = (float32x4_t){2.0f / (right - left), 0.0f, 0.0f, -(right + left) / (right - left)};
	result.rows[1] = (float32x4_t){0.0f, -2.0f / (top - bottom), 0.0f, (top + bottom) / (top - bottom)};
	result.rows[2] = (float32x4_t){0.0f, 0.0f, -1.0f / (far_plane - near_plane), -near_plane / (far_plane - near_plane)};
	result.rows[3] = (float32x4_t){0.0f, 0.0f, 0.0f, 1.0f};
	return result;
}

// ============================================================================
// Larger functions (kept at bottom for readability)
// ============================================================================

static inline float4x4 float4x4_invert(float4x4 m) {
	float inv[16];
	float det;

	// Compute cofactors (standard matrix inversion formula)
	inv[0] = m.m[5]  * m.m[10] * m.m[15] -
	         m.m[5]  * m.m[11] * m.m[14] -
	         m.m[9]  * m.m[6]  * m.m[15] +
	         m.m[9]  * m.m[7]  * m.m[14] +
	         m.m[13] * m.m[6]  * m.m[11] -
	         m.m[13] * m.m[7]  * m.m[10];

	inv[4] = -m.m[4]  * m.m[10] * m.m[15] +
	          m.m[4]  * m.m[11] * m.m[14] +
	          m.m[8]  * m.m[6]  * m.m[15] -
	          m.m[8]  * m.m[7]  * m.m[14] -
	          m.m[12] * m.m[6]  * m.m[11] +
	          m.m[12] * m.m[7]  * m.m[10];

	inv[8] = m.m[4]  * m.m[9] * m.m[15] -
	         m.m[4]  * m.m[11] * m.m[13] -
	         m.m[8]  * m.m[5] * m.m[15] +
	         m.m[8]  * m.m[7] * m.m[13] +
	         m.m[12] * m.m[5] * m.m[11] -
	         m.m[12] * m.m[7] * m.m[9];

	inv[12] = -m.m[4]  * m.m[9] * m.m[14] +
	           m.m[4]  * m.m[10] * m.m[13] +
	           m.m[8]  * m.m[5] * m.m[14] -
	           m.m[8]  * m.m[6] * m.m[13] -
	           m.m[12] * m.m[5] * m.m[10] +
	           m.m[12] * m.m[6] * m.m[9];

	inv[1] = -m.m[1]  * m.m[10] * m.m[15] +
	          m.m[1]  * m.m[11] * m.m[14] +
	          m.m[9]  * m.m[2] * m.m[15] -
	          m.m[9]  * m.m[3] * m.m[14] -
	          m.m[13] * m.m[2] * m.m[11] +
	          m.m[13] * m.m[3] * m.m[10];

	inv[5] = m.m[0]  * m.m[10] * m.m[15] -
	         m.m[0]  * m.m[11] * m.m[14] -
	         m.m[8]  * m.m[2] * m.m[15] +
	         m.m[8]  * m.m[3] * m.m[14] +
	         m.m[12] * m.m[2] * m.m[11] -
	         m.m[12] * m.m[3] * m.m[10];

	inv[9] = -m.m[0]  * m.m[9] * m.m[15] +
	          m.m[0]  * m.m[11] * m.m[13] +
	          m.m[8]  * m.m[1] * m.m[15] -
	          m.m[8]  * m.m[3] * m.m[13] -
	          m.m[12] * m.m[1] * m.m[11] +
	          m.m[12] * m.m[3] * m.m[9];

	inv[13] = m.m[0]  * m.m[9] * m.m[14] -
	          m.m[0]  * m.m[10] * m.m[13] -
	          m.m[8]  * m.m[1] * m.m[14] +
	          m.m[8]  * m.m[2] * m.m[13] +
	          m.m[12] * m.m[1] * m.m[10] -
	          m.m[12] * m.m[2] * m.m[9];

	inv[2] = m.m[1]  * m.m[6] * m.m[15] -
	         m.m[1]  * m.m[7] * m.m[14] -
	         m.m[5]  * m.m[2] * m.m[15] +
	         m.m[5]  * m.m[3] * m.m[14] +
	         m.m[13] * m.m[2] * m.m[7] -
	         m.m[13] * m.m[3] * m.m[6];

	inv[6] = -m.m[0]  * m.m[6] * m.m[15] +
	          m.m[0]  * m.m[7] * m.m[14] +
	          m.m[4]  * m.m[2] * m.m[15] -
	          m.m[4]  * m.m[3] * m.m[14] -
	          m.m[12] * m.m[2] * m.m[7] +
	          m.m[12] * m.m[3] * m.m[6];

	inv[10] = m.m[0]  * m.m[5] * m.m[15] -
	          m.m[0]  * m.m[7] * m.m[13] -
	          m.m[4]  * m.m[1] * m.m[15] +
	          m.m[4]  * m.m[3] * m.m[13] +
	          m.m[12] * m.m[1] * m.m[7] -
	          m.m[12] * m.m[3] * m.m[5];

	inv[14] = -m.m[0]  * m.m[5] * m.m[14] +
	           m.m[0]  * m.m[6] * m.m[13] +
	           m.m[4]  * m.m[1] * m.m[14] -
	           m.m[4]  * m.m[2] * m.m[13] -
	           m.m[12] * m.m[1] * m.m[6] +
	           m.m[12] * m.m[2] * m.m[5];

	inv[3] = -m.m[1] * m.m[6] * m.m[11] +
	          m.m[1] * m.m[7] * m.m[10] +
	          m.m[5] * m.m[2] * m.m[11] -
	          m.m[5] * m.m[3] * m.m[10] -
	          m.m[9] * m.m[2] * m.m[7] +
	          m.m[9] * m.m[3] * m.m[6];

	inv[7] = m.m[0] * m.m[6] * m.m[11] -
	         m.m[0] * m.m[7] * m.m[10] -
	         m.m[4] * m.m[2] * m.m[11] +
	         m.m[4] * m.m[3] * m.m[10] +
	         m.m[8] * m.m[2] * m.m[7] -
	         m.m[8] * m.m[3] * m.m[6];

	inv[11] = -m.m[0] * m.m[5] * m.m[11] +
	           m.m[0] * m.m[7] * m.m[9] +
	           m.m[4] * m.m[1] * m.m[11] -
	           m.m[4] * m.m[3] * m.m[9] -
	           m.m[8] * m.m[1] * m.m[7] +
	           m.m[8] * m.m[3] * m.m[5];

	inv[15] = m.m[0] * m.m[5] * m.m[10] -
	          m.m[0] * m.m[6] * m.m[9] -
	          m.m[4] * m.m[1] * m.m[10] +
	          m.m[4] * m.m[2] * m.m[9] +
	          m.m[8] * m.m[1] * m.m[6] -
	          m.m[8] * m.m[2] * m.m[5];

	det = m.m[0] * inv[0] + m.m[1] * inv[4] + m.m[2] * inv[8] + m.m[3] * inv[12];

	if (det == 0.0f) {
		return float4x4_identity();
	}

	det = 1.0f / det;

	float4x4 result;
	for (int i = 0; i < 16; i++) {
		result.m[i] = inv[i] * det;
	}

	return result;
}

#ifdef __cplusplus
}
#endif


#endif // FLOAT_MATH_USE_NEON

#endif // FLOAT_MATH_AMALGAMATED_H
