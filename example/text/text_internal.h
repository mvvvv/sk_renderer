// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Nick Klingensmith

#pragma once

#include <stdint.h>
#include <stdlib.h>

// GPU-friendly data structures for vector text rendering.
// All structures are designed for efficient GPU buffer layout.

///////////////////////////////////////////////////////////////////////////////
// GPU Buffer Structures (must match shader exactly)
///////////////////////////////////////////////////////////////////////////////

// Quadratic Bezier curve (3 control points)
// Curves are stored in glyph-local coordinates (normalized to units_per_em)
typedef struct {
	float p0[2];        // Start point
	float p1[2];        // Control point
	float p2[2];        // End point
	float y_min;        // Minimum Y of curve bounding box
	float y_max;        // Maximum Y of curve bounding box
} text_curve_t;         // 32 bytes, 16-byte aligned

// Horizontal band - references curves that cross this Y range
// Bands enable O(n/bands) curve testing instead of O(n) per pixel
typedef struct {
	uint32_t curve_start;   // Index into curve array
	uint32_t curve_count;   // Number of curves in this band
} text_band_t;              // 8 bytes

// Per-glyph metadata stored in GPU buffer
typedef struct {
	uint32_t band_start;    // Index into bands array (TEXT_BAND_COUNT bands per glyph)
	uint32_t curve_start;   // Index into curve array (for fallback/all curves)
	uint32_t curve_count;   // Total number of curves for this glyph
	uint32_t _pad0;         // Padding for alignment
	float    bounds_min[2]; // Glyph bounding box min (glyph space)
	float    bounds_max[2]; // Glyph bounding box max (glyph space)
	float    advance;       // Horizontal advance width
	float    lsb;           // Left side bearing
} text_glyph_gpu_t;         // 40 bytes

// Per-character instance data (uploaded each frame)
// Must match HLSL Instance struct exactly
// HLSL float4 requires 16-byte alignment!
typedef struct {
	float    transform[16]; // 4x4 world transform matrix (row-major) - 64 bytes, offset 0
	uint32_t glyph_index;   // Index into glyph buffer - 4 bytes, offset 64
	uint32_t _pad0;         // Padding - 4 bytes, offset 68
	uint32_t _pad1;         // Padding - 4 bytes, offset 72
	uint32_t _pad2;         // Padding - 4 bytes, offset 76
	float    color[4];      // RGBA color - 16 bytes, offset 80 (16-byte aligned)
} text_instance_t;          // 96 bytes total

_Static_assert(sizeof(text_instance_t) == 96, "text_instance_t must be exactly 96 bytes to match HLSL");

///////////////////////////////////////////////////////////////////////////////
// CPU-side Structures
///////////////////////////////////////////////////////////////////////////////

// Extended glyph info kept on CPU for layout calculations
typedef struct {
	text_glyph_gpu_t gpu;       // Data that goes to GPU
	int32_t          codepoint; // Unicode codepoint
	int32_t          stb_glyph; // stb_truetype glyph index
} text_glyph_t;

// Dynamic array for collecting curves/bands during font loading
typedef struct {
	void*   data;
	int32_t count;
	int32_t capacity;
	int32_t elem_size;
} text_array_t;

///////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////

#define TEXT_BAND_COUNT      16    // Number of horizontal bands per glyph
#define TEXT_ASCII_START     32    // First ASCII character (space)
#define TEXT_ASCII_END       127   // Last ASCII character (DEL excluded)
#define TEXT_ASCII_COUNT     (TEXT_ASCII_END - TEXT_ASCII_START)
#define TEXT_MAX_INSTANCES   4096  // Max characters per text_render() call

///////////////////////////////////////////////////////////////////////////////
// Internal Array Helpers
///////////////////////////////////////////////////////////////////////////////

static inline void _text_array_init(text_array_t* arr, int32_t elem_size) {
	arr->data      = NULL;
	arr->count     = 0;
	arr->capacity  = 0;
	arr->elem_size = elem_size;
}

static inline void _text_array_free(text_array_t* arr) {
	if (arr->data) {
		free(arr->data);
		arr->data     = NULL;
		arr->count    = 0;
		arr->capacity = 0;
	}
}

static inline void* _text_array_push(text_array_t* arr) {
	if (arr->count >= arr->capacity) {
		arr->capacity = arr->capacity == 0 ? 64 : arr->capacity * 2;
		arr->data     = realloc(arr->data, arr->capacity * arr->elem_size);
	}
	void* ptr = (char*)arr->data + arr->count * arr->elem_size;
	arr->count++;
	return ptr;
}

static inline void* _text_array_at(text_array_t* arr, int32_t index) {
	return (char*)arr->data + index * arr->elem_size;
}
