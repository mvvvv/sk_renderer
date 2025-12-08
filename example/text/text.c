// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Nick Klingensmith

#include "text.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

///////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////

#define TEXT_BAND_COUNT       32     // Number of horizontal bands per glyph
#define TEXT_MAX_INSTANCES    4096   // Max characters per text_render() call
#define TEXT_INITIAL_GLYPHS   256    // Initial glyph capacity
#define TEXT_INITIAL_CURVES   4096   // Initial curve capacity
#define TEXT_HASH_LOAD_FACTOR 0.7f   // Hash map load factor before resize

// Cubic-to-quadratic conversion method toggle:
// 0 = Simple midpoint averaging (original, faster but less accurate)
// 1 = Split into two quadratics (better quality, recommended for cubic fonts)
#define TEXT_CUBIC_SPLIT_METHOD 1

///////////////////////////////////////////////////////////////////////////////
// UTF-8 Decoding
///////////////////////////////////////////////////////////////////////////////

// Decode next UTF-8 codepoint and advance pointer.
// Returns 0 on end of string or invalid sequence.
static inline uint32_t _utf8_next(const char** str) {
	const uint8_t* s = (const uint8_t*)*str;
	if (!*s) return 0;

	uint32_t c = *s++;

	if (c < 0x80) {
		// ASCII - fast path (single byte)
		*str = (const char*)s;
		return c;
	}
	else if ((c & 0xE0) == 0xC0) {
		// 2-byte sequence (U+0080 - U+07FF)
		if ((*s & 0xC0) != 0x80) { *str = (const char*)s; return 0xFFFD; }
		c = ((c & 0x1F) << 6) | (*s++ & 0x3F);
	}
	else if ((c & 0xF0) == 0xE0) {
		// 3-byte sequence (U+0800 - U+FFFF)
		if ((*s & 0xC0) != 0x80) { *str = (const char*)s; return 0xFFFD; }
		c = ((c & 0x0F) << 12) | ((*s++ & 0x3F) << 6);
		if ((*s & 0xC0) != 0x80) { *str = (const char*)s; return 0xFFFD; }
		c |= (*s++ & 0x3F);
	}
	else if ((c & 0xF8) == 0xF0) {
		// 4-byte sequence (U+10000 - U+10FFFF)
		if ((*s & 0xC0) != 0x80) { *str = (const char*)s; return 0xFFFD; }
		c = ((c & 0x07) << 18) | ((*s++ & 0x3F) << 12);
		if ((*s & 0xC0) != 0x80) { *str = (const char*)s; return 0xFFFD; }
		c |= ((*s++ & 0x3F) << 6);
		if ((*s & 0xC0) != 0x80) { *str = (const char*)s; return 0xFFFD; }
		c |= (*s++ & 0x3F);
	}
	else {
		// Invalid leading byte
		*str = (const char*)s;
		return 0xFFFD;  // Replacement character
	}

	*str = (const char*)s;
	return c;
}

///////////////////////////////////////////////////////////////////////////////
// UTF-16 Decoding
///////////////////////////////////////////////////////////////////////////////

// Decode next UTF-16 codepoint and advance pointer.
// Returns 0 on end of string (null terminator).
static inline uint32_t _utf16_next(const uint16_t** str) {
	const uint16_t* s = *str;
	if (!*s) return 0;

	uint32_t c = *s++;

	// Check for surrogate pair (U+10000 and above)
	if (c >= 0xD800 && c <= 0xDBFF) {
		// High surrogate - expect low surrogate
		uint16_t low = *s;
		if (low >= 0xDC00 && low <= 0xDFFF) {
			s++;
			c = 0x10000 + (((c & 0x3FF) << 10) | (low & 0x3FF));
		} else {
			// Invalid surrogate pair
			c = 0xFFFD;
		}
	}
	else if (c >= 0xDC00 && c <= 0xDFFF) {
		// Unexpected low surrogate
		c = 0xFFFD;
	}

	*str = s;
	return c;
}

///////////////////////////////////////////////////////////////////////////////
// GPU Buffer Structures (must match shader exactly)
///////////////////////////////////////////////////////////////////////////////

// Quadratic Bezier curve (3 control points + AABB)
typedef struct {
	float p0[2];        // Start point
	float p1[2];        // Control point
	float p2[2];        // End point
	float x_min;        // Curve bounding box
	float x_max;
	float y_min;
	float y_max;
} text_curve_t;         // 40 bytes

// Per-glyph metadata stored in GPU buffer
typedef struct {
	uint32_t curve_start;   // Base index into curve array
	uint32_t curve_count;   // Total number of curves for this glyph
	float    bounds_min[2]; // Glyph bounding box min (glyph space)
	float    bounds_max[2]; // Glyph bounding box max (glyph space)
	float    advance;       // Horizontal advance width
	float    lsb;           // Left side bearing
	uint32_t bands[TEXT_BAND_COUNT]; // Packed (offset << 16) | count per band
} text_glyph_gpu_t;     // 96 bytes

// Per-character instance data (uploaded each frame)
typedef struct {
	float    pos[3];        // World position
	uint32_t glyph_index;   // Index into glyph buffer
	float    right[3];      // X axis * scale
	uint32_t color;         // Packed RGBA8
	float    up[3];         // Y axis * scale
	uint32_t _pad;
} text_instance_t;      // 48 bytes

_Static_assert(sizeof(text_instance_t) == 48, "text_instance_t must be 48 bytes");

///////////////////////////////////////////////////////////////////////////////
// CPU-side Structures
///////////////////////////////////////////////////////////////////////////////

// Extended glyph info kept on CPU
typedef struct {
	text_glyph_gpu_t gpu;       // Data that goes to GPU
	uint32_t         codepoint; // Unicode codepoint
	int32_t          stb_glyph; // stb_truetype glyph index
	uint32_t         gpu_index; // Index in GPU glyph buffer
} text_glyph_t;

// Dynamic array
typedef struct {
	void*   data;
	int32_t count;
	int32_t capacity;
	int32_t elem_size;
} text_array_t;

// Hash map entry for codepoint -> glyph index
typedef struct {
	uint32_t key;       // Codepoint (0 = empty slot)
	uint32_t value;     // Index into glyphs array
} text_hash_entry_t;

///////////////////////////////////////////////////////////////////////////////
// Array Helpers
///////////////////////////////////////////////////////////////////////////////

static inline void _array_init(text_array_t* arr, int32_t elem_size) {
	arr->data      = NULL;
	arr->count     = 0;
	arr->capacity  = 0;
	arr->elem_size = elem_size;
}

static inline void _array_free(text_array_t* arr) {
	free(arr->data);
	arr->data     = NULL;
	arr->count    = 0;
	arr->capacity = 0;
}

static inline void _array_reserve(text_array_t* arr, int32_t capacity) {
	if (arr->capacity >= capacity) return;
	arr->capacity = capacity;
	arr->data     = realloc(arr->data, arr->capacity * arr->elem_size);
}

static inline void* _array_push(text_array_t* arr) {
	if (arr->count >= arr->capacity) {
		arr->capacity = arr->capacity == 0 ? 64 : arr->capacity * 2;
		arr->data     = realloc(arr->data, arr->capacity * arr->elem_size);
	}
	void* ptr = (char*)arr->data + arr->count * arr->elem_size;
	arr->count++;
	return ptr;
}

static inline void* _array_at(text_array_t* arr, int32_t index) {
	return (char*)arr->data + index * arr->elem_size;
}

///////////////////////////////////////////////////////////////////////////////
// Hash Map (Open Addressing)
///////////////////////////////////////////////////////////////////////////////

static inline uint32_t _hash_codepoint(uint32_t cp) {
	// FNV-1a inspired
	cp ^= cp >> 16;
	cp *= 0x85ebca6b;
	cp ^= cp >> 13;
	cp *= 0xc2b2ae35;
	cp ^= cp >> 16;
	return cp;
}

typedef struct {
	text_hash_entry_t* entries;
	uint32_t           size;      // Power of 2
	uint32_t           count;
} text_glyph_map_t;

static void _glyph_map_init(text_glyph_map_t* map, uint32_t initial_size) {
	// Round up to power of 2
	uint32_t size = 64;
	while (size < initial_size) size *= 2;

	map->entries = calloc(size, sizeof(text_hash_entry_t));
	map->size    = size;
	map->count   = 0;
}

static void _glyph_map_free(text_glyph_map_t* map) {
	free(map->entries);
	map->entries = NULL;
	map->size    = 0;
	map->count   = 0;
}

static void _glyph_map_grow(text_glyph_map_t* map) {
	uint32_t           old_size    = map->size;
	text_hash_entry_t* old_entries = map->entries;

	map->size    = old_size * 2;
	map->entries = calloc(map->size, sizeof(text_hash_entry_t));
	map->count   = 0;

	// Rehash all entries
	uint32_t mask = map->size - 1;
	for (uint32_t i = 0; i < old_size; i++) {
		if (old_entries[i].key != 0) {
			uint32_t idx = _hash_codepoint(old_entries[i].key) & mask;
			while (map->entries[idx].key != 0) {
				idx = (idx + 1) & mask;
			}
			map->entries[idx] = old_entries[i];
			map->count++;
		}
	}

	free(old_entries);
}

// Returns pointer to value if found, NULL otherwise
static uint32_t* _glyph_map_get(text_glyph_map_t* map, uint32_t codepoint) {
	if (codepoint == 0 || map->size == 0) return NULL;

	uint32_t mask = map->size - 1;
	uint32_t idx  = _hash_codepoint(codepoint) & mask;

	for (uint32_t i = 0; i < map->size; i++) {
		if (map->entries[idx].key == codepoint) {
			return &map->entries[idx].value;
		}
		if (map->entries[idx].key == 0) {
			return NULL;  // Empty slot = not found
		}
		idx = (idx + 1) & mask;
	}
	return NULL;
}

static void _glyph_map_insert(text_glyph_map_t* map, uint32_t codepoint, uint32_t value) {
	if (codepoint == 0) return;  // 0 is reserved for empty

	// Grow if load factor exceeded
	if ((float)map->count / map->size > TEXT_HASH_LOAD_FACTOR) {
		_glyph_map_grow(map);
	}

	uint32_t mask = map->size - 1;
	uint32_t idx  = _hash_codepoint(codepoint) & mask;

	while (map->entries[idx].key != 0 && map->entries[idx].key != codepoint) {
		idx = (idx + 1) & mask;
	}

	if (map->entries[idx].key == 0) {
		map->count++;
	}
	map->entries[idx].key   = codepoint;
	map->entries[idx].value = value;
}

///////////////////////////////////////////////////////////////////////////////
// Font Structure
///////////////////////////////////////////////////////////////////////////////

struct text_font_t {
	// stb_truetype data
	stbtt_fontinfo stb_font;
	void*          ttf_data;

	// Font metrics (normalized to 1.0 = ascent height)
	float units_per_em;
	float ascent;
	float descent;
	float line_gap;
	float scale;  // 1.0 / units_per_em

	// Fast path: ASCII glyphs (direct lookup, no hashing)
	text_glyph_t* ascii[128];  // Pointers into glyphs array (NULL if not loaded)

	// Extended: hash map for non-ASCII codepoints (U+0080 and above)
	text_glyph_map_t glyph_map;

	// All loaded glyphs (dynamic array)
	text_glyph_t* glyphs;
	uint32_t      glyph_count;
	uint32_t      glyph_capacity;

	// CPU copies of GPU data (for buffer regrowth)
	text_array_t curves_cpu;      // text_curve_t
	text_array_t glyphs_gpu_cpu;  // text_glyph_gpu_t

	// GPU buffers
	skr_buffer_t curve_buffer;
	skr_buffer_t glyph_buffer;
	bool         gpu_dirty;

	bool valid;
};

///////////////////////////////////////////////////////////////////////////////
// Context Structure
///////////////////////////////////////////////////////////////////////////////

struct text_context_t {
	text_font_t*    font;
	skr_shader_t*   shader;
	skr_material_t* material;

	text_instance_t instances[TEXT_MAX_INSTANCES];
	int32_t         instance_count;

	skr_mesh_t      quad_mesh;
	skr_vert_type_t quad_vertex_type;
};

///////////////////////////////////////////////////////////////////////////////
// Curve Extraction
///////////////////////////////////////////////////////////////////////////////

// Weld curve endpoints to eliminate floating-point gaps.
// This ensures that when curves share endpoints, they have EXACTLY the same
// floating-point values, preventing winding calculation errors at junctions.
// Processes curves contour by contour (contours start when p0 doesn't match
// the previous curve's p2).
//
// Returns: number of welds performed (for debugging)
static int32_t _weld_curve_endpoints(text_curve_t* curves, int32_t count) {
	if (count < 2) return 0;

	int32_t welds = 0;
	int32_t contour_start = 0;

	// More aggressive threshold for contour detection
	// Fonts can have surprisingly large gaps at contour boundaries
	const float CONTOUR_BREAK_THRESHOLD = 0.1f;  // 10% of unit square

	for (int32_t i = 1; i <= count; i++) {
		// Detect contour boundary: new contour starts when p0 is far from prev p2
		bool is_contour_end = (i == count);
		if (!is_contour_end) {
			float dx = curves[i].p0[0] - curves[i-1].p2[0];
			float dy = curves[i].p0[1] - curves[i-1].p2[1];
			is_contour_end = (dx * dx + dy * dy > CONTOUR_BREAK_THRESHOLD * CONTOUR_BREAK_THRESHOLD);
		}

		if (is_contour_end) {
			int32_t contour_end = i;
			int32_t contour_len = contour_end - contour_start;

			if (contour_len >= 2) {
				// ALWAYS weld sequential endpoints within contour
				// Don't check distance - just force them to match
				for (int32_t j = contour_start; j < contour_end - 1; j++) {
					float dx = curves[j+1].p0[0] - curves[j].p2[0];
					float dy = curves[j+1].p0[1] - curves[j].p2[1];
					if (dx != 0.0f || dy != 0.0f) {
						curves[j+1].p0[0] = curves[j].p2[0];
						curves[j+1].p0[1] = curves[j].p2[1];
						welds++;
					}
				}

				// Weld contour closure: last p2 -> first p0
				text_curve_t* first = &curves[contour_start];
				text_curve_t* last  = &curves[contour_end - 1];
				float dx = last->p2[0] - first->p0[0];
				float dy = last->p2[1] - first->p0[1];
				// More aggressive closure threshold
				if (dx * dx + dy * dy < CONTOUR_BREAK_THRESHOLD * CONTOUR_BREAK_THRESHOLD) {
					if (dx != 0.0f || dy != 0.0f) {
						last->p2[0] = first->p0[0];
						last->p2[1] = first->p0[1];
						welds++;
					}
				}
			}

			contour_start = i;
		}
	}

	return welds;
}

// Compute tight AABB for a quadratic Bezier curve
static void _compute_curve_aabb(
	float p0x, float p0y, float p1x, float p1y, float p2x, float p2y,
	float* out_x_min, float* out_x_max, float* out_y_min, float* out_y_max
) {
	float ax = p0x - 2.0f * p1x + p2x;
	float ay = p0y - 2.0f * p1y + p2y;
	float bx = p1x - p0x;
	float by = p1y - p0y;

	float tx = (fabsf(ax) > 1e-8f) ? fminf(fmaxf(-bx / ax, 0.0f), 1.0f) : 0.0f;
	float ty = (fabsf(ay) > 1e-8f) ? fminf(fmaxf(-by / ay, 0.0f), 1.0f) : 0.0f;

	float qx = p0x + tx * (2.0f * bx + tx * ax);
	float qy = p0y + ty * (2.0f * by + ty * ay);

	*out_x_min = fminf(fminf(p0x, p2x), qx);
	*out_x_max = fmaxf(fmaxf(p0x, p2x), qx);
	*out_y_min = fminf(fminf(p0y, p2y), qy);
	*out_y_max = fmaxf(fmaxf(p0y, p2y), qy);
}

// Evaluate quadratic Bezier at parameter t
static inline void _bezier_eval(
	float p0x, float p0y, float p1x, float p1y, float p2x, float p2y,
	float t, float* out_x, float* out_y
) {
	float it = 1.0f - t;
	*out_x = it * it * p0x + 2.0f * it * t * p1x + t * t * p2x;
	*out_y = it * it * p0y + 2.0f * it * t * p1y + t * t * p2y;
}

// Split quadratic Bezier at parameter t using de Casteljau
static inline void _bezier_split(
	float p0x, float p0y, float p1x, float p1y, float p2x, float p2y,
	float t,
	float* a0x, float* a0y, float* a1x, float* a1y, float* a2x, float* a2y,
	float* b0x, float* b0y, float* b1x, float* b1y, float* b2x, float* b2y
) {
	// First split point
	float m0x = p0x + t * (p1x - p0x);
	float m0y = p0y + t * (p1y - p0y);
	float m1x = p1x + t * (p2x - p1x);
	float m1y = p1y + t * (p2y - p1y);
	// Point on curve
	float mx = m0x + t * (m1x - m0x);
	float my = m0y + t * (m1y - m0y);

	// First curve: p0 -> m0 -> m
	*a0x = p0x; *a0y = p0y;
	*a1x = m0x; *a1y = m0y;
	*a2x = mx;  *a2y = my;

	// Second curve: m -> m1 -> p2
	*b0x = mx;  *b0y = my;
	*b1x = m1x; *b1y = m1y;
	*b2x = p2x; *b2y = p2y;
}

// Make curve monotonic in Y by splitting at Y-extremum if needed.
// Monotonic curves are essential for robust winding calculation.
// Returns number of curves output (1 or 2).
static int32_t _make_monotonic_y(
	float p0x, float p0y, float p1x, float p1y, float p2x, float p2y,
	text_curve_t* out_curves
) {
	// Quadratic coefficients for y(t) = a*t^2 + b*t + c
	float ay = p0y - 2.0f * p1y + p2y;
	float by = p1y - p0y;

	// Find t where dy/dt = 0: t = -b / (2a)
	// If this t is in (0, 1), split there
	if (fabsf(ay) > 1e-8f) {
		float t_ext = -by / ay;
		if (t_ext > 0.001f && t_ext < 0.999f) {
			// Split into two monotonic curves
			float a0x, a0y, a1x, a1y, a2x, a2y;
			float b0x, b0y, b1x, b1y, b2x, b2y;
			_bezier_split(p0x, p0y, p1x, p1y, p2x, p2y, t_ext,
			              &a0x, &a0y, &a1x, &a1y, &a2x, &a2y,
			              &b0x, &b0y, &b1x, &b1y, &b2x, &b2y);

			// First curve
			out_curves[0].p0[0] = a0x; out_curves[0].p0[1] = a0y;
			out_curves[0].p1[0] = a1x; out_curves[0].p1[1] = a1y;
			out_curves[0].p2[0] = a2x; out_curves[0].p2[1] = a2y;
			_compute_curve_aabb(a0x, a0y, a1x, a1y, a2x, a2y,
			                    &out_curves[0].x_min, &out_curves[0].x_max,
			                    &out_curves[0].y_min, &out_curves[0].y_max);

			// Second curve
			out_curves[1].p0[0] = b0x; out_curves[1].p0[1] = b0y;
			out_curves[1].p1[0] = b1x; out_curves[1].p1[1] = b1y;
			out_curves[1].p2[0] = b2x; out_curves[1].p2[1] = b2y;
			_compute_curve_aabb(b0x, b0y, b1x, b1y, b2x, b2y,
			                    &out_curves[1].x_min, &out_curves[1].x_max,
			                    &out_curves[1].y_min, &out_curves[1].y_max);

			return 2;
		}
	}

	// Already monotonic
	out_curves[0].p0[0] = p0x; out_curves[0].p0[1] = p0y;
	out_curves[0].p1[0] = p1x; out_curves[0].p1[1] = p1y;
	out_curves[0].p2[0] = p2x; out_curves[0].p2[1] = p2y;
	_compute_curve_aabb(p0x, p0y, p1x, p1y, p2x, p2y,
	                    &out_curves[0].x_min, &out_curves[0].x_max,
	                    &out_curves[0].y_min, &out_curves[0].y_max);
	return 1;
}

// Extract curves from a glyph into temp array
static void _extract_glyph_curves(
	stbtt_fontinfo* font,
	int32_t         glyph_index,
	float           scale,
	text_array_t*   out_curves,
	float*          out_min_x,
	float*          out_max_x,
	float*          out_min_y,
	float*          out_max_y
) {
	stbtt_vertex* vertices     = NULL;
	int32_t       num_vertices = stbtt_GetGlyphShape(font, glyph_index, &vertices);

	*out_min_x =  1e10f;
	*out_max_x = -1e10f;
	*out_min_y =  1e10f;
	*out_max_y = -1e10f;

	if (num_vertices == 0 || vertices == NULL) return;

	float cx = 0, cy = 0;

	for (int32_t i = 0; i < num_vertices; i++) {
		stbtt_vertex* v = &vertices[i];
		float x   = v->x  * scale;
		float y   = v->y  * scale;
		float cx1 = v->cx * scale;
		float cy1 = v->cy * scale;

		switch (v->type) {
		case STBTT_vmove:
			cx = x; cy = y;
			break;

		case STBTT_vline: {
			// Lines are always monotonic, just add directly
			text_curve_t* curve = _array_push(out_curves);
			curve->p0[0] = cx;
			curve->p0[1] = cy;
			curve->p1[0] = (cx + x) * 0.5f;
			curve->p1[1] = (cy + y) * 0.5f;
			curve->p2[0] = x;
			curve->p2[1] = y;
			_compute_curve_aabb(curve->p0[0], curve->p0[1], curve->p1[0], curve->p1[1],
			                    curve->p2[0], curve->p2[1], &curve->x_min, &curve->x_max,
			                    &curve->y_min, &curve->y_max);
			*out_min_x = fminf(*out_min_x, curve->x_min);
			*out_max_x = fmaxf(*out_max_x, curve->x_max);
			*out_min_y = fminf(*out_min_y, curve->y_min);
			*out_max_y = fmaxf(*out_max_y, curve->y_max);
			cx = x; cy = y;
		} break;

		case STBTT_vcurve: {
			// Split quadratic at Y-extremum to make monotonic
			text_curve_t mono[2];
			int32_t n = _make_monotonic_y(cx, cy, cx1, cy1, x, y, mono);
			for (int32_t j = 0; j < n; j++) {
				text_curve_t* curve = _array_push(out_curves);
				*curve = mono[j];
				*out_min_x = fminf(*out_min_x, curve->x_min);
				*out_max_x = fmaxf(*out_max_x, curve->x_max);
				*out_min_y = fminf(*out_min_y, curve->y_min);
				*out_max_y = fmaxf(*out_max_y, curve->y_max);
			}
			cx = x; cy = y;
		} break;

		case STBTT_vcubic: {
			float cx2 = v->cx1 * scale;
			float cy2 = v->cy1 * scale;

#if TEXT_CUBIC_SPLIT_METHOD == 0
			// Simple midpoint averaging (original method)
			// Faster but less accurate for curved segments
			float qp1x = (cx1 + cx2) * 0.5f;
			float qp1y = (cy1 + cy2) * 0.5f;

			// Make monotonic
			text_curve_t mono[2];
			int32_t n = _make_monotonic_y(cx, cy, qp1x, qp1y, x, y, mono);
			for (int32_t j = 0; j < n; j++) {
				text_curve_t* curve = _array_push(out_curves);
				*curve = mono[j];
				*out_min_x = fminf(*out_min_x, curve->x_min);
				*out_max_x = fmaxf(*out_max_x, curve->x_max);
				*out_min_y = fminf(*out_min_y, curve->y_min);
				*out_max_y = fmaxf(*out_max_y, curve->y_max);
			}
#else
			// Split cubic into two quadratics (better approximation)
			// Uses 0.75 interpolation which minimizes error for typical curves
			// Reference: osor.io "Rendering Crispy Text On The GPU"
			float c0x = cx  + 0.75f * (cx1 - cx);
			float c0y = cy  + 0.75f * (cy1 - cy);
			float c1x = x   + 0.75f * (cx2 - x);
			float c1y = y   + 0.75f * (cy2 - y);
			float mx  = 0.5f * (c0x + c1x);
			float my  = 0.5f * (c0y + c1y);

			// First quadratic: start -> c0 -> midpoint (make monotonic)
			text_curve_t mono1[2];
			int32_t n1 = _make_monotonic_y(cx, cy, c0x, c0y, mx, my, mono1);
			for (int32_t j = 0; j < n1; j++) {
				text_curve_t* curve = _array_push(out_curves);
				*curve = mono1[j];
				*out_min_x = fminf(*out_min_x, curve->x_min);
				*out_max_x = fmaxf(*out_max_x, curve->x_max);
				*out_min_y = fminf(*out_min_y, curve->y_min);
				*out_max_y = fmaxf(*out_max_y, curve->y_max);
			}

			// Second quadratic: midpoint -> c1 -> end (make monotonic)
			text_curve_t mono2[2];
			int32_t n2 = _make_monotonic_y(mx, my, c1x, c1y, x, y, mono2);
			for (int32_t j = 0; j < n2; j++) {
				text_curve_t* curve = _array_push(out_curves);
				*curve = mono2[j];
				*out_min_x = fminf(*out_min_x, curve->x_min);
				*out_max_x = fmaxf(*out_max_x, curve->x_max);
				*out_min_y = fminf(*out_min_y, curve->y_min);
				*out_max_y = fmaxf(*out_max_y, curve->y_max);
			}
#endif
			cx = x; cy = y;
		} break;
		}
	}

	stbtt_FreeShape(font, vertices);
}

// Organize curves into bands for a single glyph
static void _organize_into_bands(
	text_curve_t*     glyph_curves,
	int32_t           curve_count,
	float             glyph_y_min,
	float             glyph_y_max,
	text_glyph_gpu_t* out_glyph,
	text_array_t*     out_band_curves
) {
	uint32_t glyph_curve_start = (uint32_t)out_band_curves->count;
	out_glyph->curve_start = glyph_curve_start;

	if (curve_count == 0) {
		for (int32_t b = 0; b < TEXT_BAND_COUNT; b++) {
			out_glyph->bands[b] = 0;
		}
		out_glyph->curve_count = 0;
		return;
	}

	float glyph_height = glyph_y_max - glyph_y_min;
	if (glyph_height < 1e-6f) glyph_height = 1.0f;
	float band_height = glyph_height / TEXT_BAND_COUNT;

	// Add small overlap between bands to handle floating-point precision
	// at band boundaries. This ensures curves touching the boundary are
	// included in both adjacent bands, preventing missed intersections.
	float band_overlap = band_height * 0.01f;

	for (int32_t b = 0; b < TEXT_BAND_COUNT; b++) {
		float band_y_min = glyph_y_min + b * band_height - band_overlap;
		float band_y_max = glyph_y_min + (b + 1) * band_height + band_overlap;

		uint32_t band_offset = (uint32_t)out_band_curves->count - glyph_curve_start;
		uint32_t band_count  = 0;

		for (int32_t c = 0; c < curve_count; c++) {
			text_curve_t* curve = &glyph_curves[c];
			if (curve->y_max >= band_y_min && curve->y_min <= band_y_max) {
				text_curve_t* band_curve = _array_push(out_band_curves);
				*band_curve = *curve;
				band_count++;
			}
		}

		out_glyph->bands[b] = (band_offset << 16) | (band_count & 0xFFFF);
	}

	out_glyph->curve_count = (uint32_t)out_band_curves->count - glyph_curve_start;
}

///////////////////////////////////////////////////////////////////////////////
// Lazy Glyph Loading
///////////////////////////////////////////////////////////////////////////////

// Load a single glyph on demand. Returns NULL if glyph not in font.
static text_glyph_t* _load_glyph(text_font_t* font, uint32_t codepoint) {
	int32_t stb_idx = stbtt_FindGlyphIndex(&font->stb_font, codepoint);
	if (stb_idx == 0 && codepoint != 0) {
		return NULL;  // Glyph not in font
	}

	// Grow glyph array if needed
	if (font->glyph_count >= font->glyph_capacity) {
		font->glyph_capacity = font->glyph_capacity ? font->glyph_capacity * 2 : TEXT_INITIAL_GLYPHS;
		font->glyphs = realloc(font->glyphs, font->glyph_capacity * sizeof(text_glyph_t));
	}

	text_glyph_t* glyph = &font->glyphs[font->glyph_count];
	glyph->codepoint = codepoint;
	glyph->stb_glyph = stb_idx;
	glyph->gpu_index = font->glyph_count;

	// Get metrics
	int32_t advance, lsb;
	stbtt_GetGlyphHMetrics(&font->stb_font, stb_idx, &advance, &lsb);
	glyph->gpu.advance = advance * font->scale;
	glyph->gpu.lsb     = lsb * font->scale;

	// Extract curves into temp array and compute actual bounds
	text_array_t temp_curves;
	_array_init(&temp_curves, sizeof(text_curve_t));

	float glyph_min_x, glyph_max_x, glyph_min_y, glyph_max_y;
	_extract_glyph_curves(&font->stb_font, stb_idx, font->scale, &temp_curves,
	                      &glyph_min_x, &glyph_max_x, &glyph_min_y, &glyph_max_y);

	// Note: Welding is no longer needed - monotonic curves with asymmetric
	// endpoint comparisons handle shared endpoints correctly.

	// Use actual curve bounds for both quad and bands (no padding)
	// This ensures perfect alignment between shader band lookup and curve data
	if (temp_curves.count > 0) {
		glyph->gpu.bounds_min[0] = glyph_min_x;
		glyph->gpu.bounds_min[1] = glyph_min_y;
		glyph->gpu.bounds_max[0] = glyph_max_x;
		glyph->gpu.bounds_max[1] = glyph_max_y;
	} else {
		// Fallback for glyphs without curves (space, etc.)
		int32_t x0, y0, x1, y1;
		stbtt_GetGlyphBox(&font->stb_font, stb_idx, &x0, &y0, &x1, &y1);
		glyph->gpu.bounds_min[0] = x0 * font->scale;
		glyph->gpu.bounds_min[1] = y0 * font->scale;
		glyph->gpu.bounds_max[0] = x1 * font->scale;
		glyph->gpu.bounds_max[1] = y1 * font->scale;
	}

	// Organize bands using the same bounds
	_organize_into_bands(
		(text_curve_t*)temp_curves.data,
		temp_curves.count,
		glyph->gpu.bounds_min[1],
		glyph->gpu.bounds_max[1],
		&glyph->gpu,
		&font->curves_cpu
	);

	_array_free(&temp_curves);

	// Add GPU glyph data to CPU array
	text_glyph_gpu_t* gpu_glyph = _array_push(&font->glyphs_gpu_cpu);
	*gpu_glyph = glyph->gpu;

	font->glyph_count++;
	font->gpu_dirty = true;

	return glyph;
}

// Get glyph for codepoint, loading if necessary
static inline text_glyph_t* _get_glyph(text_font_t* font, uint32_t codepoint) {
	// Fast path: ASCII (direct array lookup)
	if (codepoint < 128) {
		text_glyph_t* g = font->ascii[codepoint];
		if (!g) {
			g = _load_glyph(font, codepoint);
			font->ascii[codepoint] = g;
		}
		return g;
	}

	// Hash map lookup for extended characters
	uint32_t* idx = _glyph_map_get(&font->glyph_map, codepoint);
	if (idx) {
		return &font->glyphs[*idx];
	}

	// Not cached - load from font
	text_glyph_t* g = _load_glyph(font, codepoint);
	if (g) {
		_glyph_map_insert(&font->glyph_map, codepoint, g->gpu_index);
	}
	return g;
}

///////////////////////////////////////////////////////////////////////////////
// GPU Buffer Management
///////////////////////////////////////////////////////////////////////////////

static void _sync_gpu_buffers(text_font_t* font) {
	if (!font->gpu_dirty) return;

	// Destroy old buffers (deferred destruction handles in-flight frames)
	if (skr_buffer_is_valid(&font->curve_buffer)) skr_buffer_destroy(&font->curve_buffer);
	if (skr_buffer_is_valid(&font->glyph_buffer)) skr_buffer_destroy(&font->glyph_buffer);

	// Create new buffers from CPU data
	if (font->curves_cpu.count > 0) {
		skr_buffer_create(font->curves_cpu.data, font->curves_cpu.count, sizeof(text_curve_t),
		                  skr_buffer_type_storage, skr_use_static, &font->curve_buffer);
		skr_buffer_set_name(&font->curve_buffer, "text_curves");
	}

	if (font->glyphs_gpu_cpu.count > 0) {
		skr_buffer_create(font->glyphs_gpu_cpu.data, font->glyphs_gpu_cpu.count, sizeof(text_glyph_gpu_t),
		                  skr_buffer_type_storage, skr_use_static, &font->glyph_buffer);
		skr_buffer_set_name(&font->glyph_buffer, "text_glyphs");
	}

	font->gpu_dirty = false;
}

///////////////////////////////////////////////////////////////////////////////
// Font Loading
///////////////////////////////////////////////////////////////////////////////

text_font_t* text_font_load(const void* ttf_data, size_t ttf_size) {
	if (!ttf_data || ttf_size == 0) return NULL;

	text_font_t* font = calloc(1, sizeof(text_font_t));
	if (!font) return NULL;

	// Copy TTF data
	font->ttf_data = malloc(ttf_size);
	if (!font->ttf_data) {
		free(font);
		return NULL;
	}
	memcpy(font->ttf_data, ttf_data, ttf_size);

	// Initialize stb_truetype
	if (!stbtt_InitFont(&font->stb_font, font->ttf_data, 0)) {
		free(font->ttf_data);
		free(font);
		return NULL;
	}

	// Get font metrics
	int32_t ascent, descent, line_gap;
	stbtt_GetFontVMetrics(&font->stb_font, &ascent, &descent, &line_gap);

	font->units_per_em = (float)ascent;
	font->scale        = 1.0f / font->units_per_em;
	font->ascent       = ascent   * font->scale;
	font->descent      = descent  * font->scale;
	font->line_gap     = line_gap * font->scale;

	// Initialize glyph storage (empty - lazy loading)
	memset(font->ascii, 0, sizeof(font->ascii));
	_glyph_map_init(&font->glyph_map, 256);

	font->glyphs         = NULL;
	font->glyph_count    = 0;
	font->glyph_capacity = 0;

	// Initialize CPU arrays for GPU data
	_array_init(&font->curves_cpu, sizeof(text_curve_t));
	_array_init(&font->glyphs_gpu_cpu, sizeof(text_glyph_gpu_t));
	_array_reserve(&font->curves_cpu, TEXT_INITIAL_CURVES);
	_array_reserve(&font->glyphs_gpu_cpu, TEXT_INITIAL_GLYPHS);

	font->gpu_dirty = false;
	font->valid     = true;

	return font;
}

void text_font_destroy(text_font_t* font) {
	if (!font) return;

	skr_buffer_destroy(&font->curve_buffer);
	skr_buffer_destroy(&font->glyph_buffer);

	_array_free(&font->curves_cpu);
	_array_free(&font->glyphs_gpu_cpu);
	_glyph_map_free(&font->glyph_map);

	free(font->glyphs);
	free(font->ttf_data);
	free(font);
}

bool text_font_is_valid(const text_font_t* font) {
	return font && font->valid;
}

float text_font_get_ascent(const text_font_t* font) {
	return font ? font->ascent : 0;
}

float text_font_get_descent(const text_font_t* font) {
	return font ? font->descent : 0;
}

float text_font_get_line_gap(const text_font_t* font) {
	return font ? font->line_gap : 0;
}

///////////////////////////////////////////////////////////////////////////////
// Text Context
///////////////////////////////////////////////////////////////////////////////

text_context_t* text_context_create(text_font_t* font, skr_shader_t* shader, skr_material_t* material) {
	if (!font || !font->valid) return NULL;

	text_context_t* ctx = calloc(1, sizeof(text_context_t));
	if (!ctx) return NULL;

	ctx->font     = font;
	ctx->shader   = shader;
	ctx->material = material;

	// Create quad vertex type and mesh
	typedef struct {
		float position[2];
		float uv[2];
	} quad_vertex_t;

	skr_vert_type_create(
		(skr_vert_component_t[]){
			{ .format = skr_vertex_fmt_f32, .count = 2, .semantic = skr_semantic_position, .semantic_slot = 0 },
			{ .format = skr_vertex_fmt_f32, .count = 2, .semantic = skr_semantic_texcoord, .semantic_slot = 0 }
		}, 2, &ctx->quad_vertex_type);

	quad_vertex_t quad_verts[] = {
		{ .position = {0, 0}, .uv = {0, 0} },
		{ .position = {1, 0}, .uv = {1, 0} },
		{ .position = {1, 1}, .uv = {1, 1} },
		{ .position = {0, 1}, .uv = {0, 1} },
	};
	uint16_t quad_indices[] = { 0, 1, 2, 2, 3, 0 };

	skr_mesh_create(&ctx->quad_vertex_type, skr_index_fmt_u16, quad_verts, 4, quad_indices, 6, &ctx->quad_mesh);
	skr_mesh_set_name(&ctx->quad_mesh, "text_quad");

	return ctx;
}

void text_context_destroy(text_context_t* ctx) {
	if (!ctx) return;
	skr_mesh_destroy(&ctx->quad_mesh);
	skr_vert_type_destroy(&ctx->quad_vertex_type);
	free(ctx);
}

void text_context_clear(text_context_t* ctx) {
	if (!ctx) return;
	ctx->instance_count = 0;
}

///////////////////////////////////////////////////////////////////////////////
// Text Measurement
///////////////////////////////////////////////////////////////////////////////

float text_measure_width_utf8(text_font_t* font, const char* text) {
	if (!font || !text) return 0;

	float       width          = 0;
	const char* p              = text;
	uint32_t    prev_codepoint = 0;

	while (*p) {
		uint32_t codepoint = _utf8_next(&p);
		if (codepoint == 0) break;

		text_glyph_t* glyph = _get_glyph(font, codepoint);
		if (!glyph) continue;

		// Kerning
		if (prev_codepoint) {
			int32_t kern = stbtt_GetCodepointKernAdvance(&font->stb_font, prev_codepoint, codepoint);
			width += kern * font->scale;
		}

		width += glyph->gpu.advance;
		prev_codepoint = codepoint;
	}

	return width;
}

float text_measure_width_utf16(text_font_t* font, const uint16_t* text) {
	if (!font || !text) return 0;

	float           width          = 0;
	const uint16_t* p              = text;
	uint32_t        prev_codepoint = 0;

	while (*p) {
		uint32_t codepoint = _utf16_next(&p);
		if (codepoint == 0) break;

		text_glyph_t* glyph = _get_glyph(font, codepoint);
		if (!glyph) continue;

		if (prev_codepoint) {
			int32_t kern = stbtt_GetCodepointKernAdvance(&font->stb_font, prev_codepoint, codepoint);
			width += kern * font->scale;
		}

		width += glyph->gpu.advance;
		prev_codepoint = codepoint;
	}

	return width;
}

// Backward compatible alias
float text_measure_width(const text_font_t* font, const char* text) {
	return text_measure_width_utf8((text_font_t*)font, text);
}

///////////////////////////////////////////////////////////////////////////////
// Text Adding (UTF-8)
///////////////////////////////////////////////////////////////////////////////

void text_add_utf8(
	text_context_t* ctx,
	const char*     text,
	float4x4        transform,
	float           size,
	float4          color,
	text_align_t    align
) {
	if (!ctx || !text || ctx->instance_count >= TEXT_MAX_INSTANCES) return;

	text_font_t* font  = ctx->font;
	float        scale = size;

	// Measure for alignment
	float text_width = text_measure_width_utf8(font, text) * scale;
	float offset_x   = 0;

	switch (align) {
	case text_align_left:   offset_x = 0;               break;
	case text_align_center: offset_x = -text_width / 2; break;
	case text_align_right:  offset_x = -text_width;     break;
	}

	// Process each codepoint
	float       cursor_x       = offset_x;
	const char* p              = text;
	uint32_t    prev_codepoint = 0;

	while (*p && ctx->instance_count < TEXT_MAX_INSTANCES) {
		uint32_t codepoint = _utf8_next(&p);
		if (codepoint == 0) break;

		text_glyph_t* glyph = _get_glyph(font, codepoint);
		if (!glyph) continue;

		// Kerning
		if (prev_codepoint) {
			int32_t kern = stbtt_GetCodepointKernAdvance(&font->stb_font, prev_codepoint, codepoint);
			cursor_x += kern * font->scale * scale;
		}

		// Only render glyphs with curves (skip space, etc.)
		if (glyph->gpu.curve_count > 0) {
			float4x4 local = float4x4_trs(
				(float3){ cursor_x, 0, 0 },
				(float4){ 0, 0, 0, 1 },
				(float3){ scale, scale, 1 }
			);
			float4x4 final = float4x4_mul(transform, local);

			text_instance_t* inst = &ctx->instances[ctx->instance_count++];
			inst->pos[0]   = final.m[3];
			inst->pos[1]   = final.m[7];
			inst->pos[2]   = final.m[11];
			inst->right[0] = final.m[0];
			inst->right[1] = final.m[4];
			inst->right[2] = final.m[8];
			inst->up[0]    = final.m[1];
			inst->up[1]    = final.m[5];
			inst->up[2]    = final.m[9];

			inst->glyph_index = glyph->gpu_index;
			inst->_pad        = 0;

			uint32_t r = (uint32_t)(color.x * 255.0f) & 0xFF;
			uint32_t g = (uint32_t)(color.y * 255.0f) & 0xFF;
			uint32_t b = (uint32_t)(color.z * 255.0f) & 0xFF;
			uint32_t a = (uint32_t)(color.w * 255.0f) & 0xFF;
			inst->color = r | (g << 8) | (b << 16) | (a << 24);
		}

		cursor_x += glyph->gpu.advance * scale;
		prev_codepoint = codepoint;
	}
}

///////////////////////////////////////////////////////////////////////////////
// Text Adding (UTF-16)
///////////////////////////////////////////////////////////////////////////////

void text_add_utf16(
	text_context_t*  ctx,
	const uint16_t*  text,
	float4x4         transform,
	float            size,
	float4           color,
	text_align_t     align
) {
	if (!ctx || !text || ctx->instance_count >= TEXT_MAX_INSTANCES) return;

	text_font_t* font  = ctx->font;
	float        scale = size;

	// Measure for alignment
	float text_width = text_measure_width_utf16(font, text) * scale;
	float offset_x   = 0;

	switch (align) {
	case text_align_left:   offset_x = 0;               break;
	case text_align_center: offset_x = -text_width / 2; break;
	case text_align_right:  offset_x = -text_width;     break;
	}

	// Process each codepoint
	float           cursor_x       = offset_x;
	const uint16_t* p              = text;
	uint32_t        prev_codepoint = 0;

	while (*p && ctx->instance_count < TEXT_MAX_INSTANCES) {
		uint32_t codepoint = _utf16_next(&p);
		if (codepoint == 0) break;

		text_glyph_t* glyph = _get_glyph(font, codepoint);
		if (!glyph) continue;

		// Kerning
		if (prev_codepoint) {
			int32_t kern = stbtt_GetCodepointKernAdvance(&font->stb_font, prev_codepoint, codepoint);
			cursor_x += kern * font->scale * scale;
		}

		// Only render glyphs with curves
		if (glyph->gpu.curve_count > 0) {
			float4x4 local = float4x4_trs(
				(float3){ cursor_x, 0, 0 },
				(float4){ 0, 0, 0, 1 },
				(float3){ scale, scale, 1 }
			);
			float4x4 final = float4x4_mul(transform, local);

			text_instance_t* inst = &ctx->instances[ctx->instance_count++];
			inst->pos[0]   = final.m[3];
			inst->pos[1]   = final.m[7];
			inst->pos[2]   = final.m[11];
			inst->right[0] = final.m[0];
			inst->right[1] = final.m[4];
			inst->right[2] = final.m[8];
			inst->up[0]    = final.m[1];
			inst->up[1]    = final.m[5];
			inst->up[2]    = final.m[9];

			inst->glyph_index = glyph->gpu_index;
			inst->_pad        = 0;

			uint32_t r = (uint32_t)(color.x * 255.0f) & 0xFF;
			uint32_t g = (uint32_t)(color.y * 255.0f) & 0xFF;
			uint32_t b = (uint32_t)(color.z * 255.0f) & 0xFF;
			uint32_t a = (uint32_t)(color.w * 255.0f) & 0xFF;
			inst->color = r | (g << 8) | (b << 16) | (a << 24);
		}

		cursor_x += glyph->gpu.advance * scale;
		prev_codepoint = codepoint;
	}
}

// Backward compatible alias
void text_add(
	text_context_t* ctx,
	const char*     text,
	float4x4        transform,
	float           size,
	float4          color,
	text_align_t    align
) {
	text_add_utf8(ctx, text, transform, size, color, align);
}

///////////////////////////////////////////////////////////////////////////////
// Rendering
///////////////////////////////////////////////////////////////////////////////

void text_render(text_context_t* ctx, skr_render_list_t* render_list) {
	if (!ctx || ctx->instance_count == 0) return;

	// Sync GPU buffers if glyphs were loaded this frame
	_sync_gpu_buffers(ctx->font);

	// Bind font data buffers to material
	skr_material_set_buffer(ctx->material, "curves", &ctx->font->curve_buffer);
	skr_material_set_buffer(ctx->material, "glyphs", &ctx->font->glyph_buffer);

	// Add to render list
	skr_render_list_add(render_list, &ctx->quad_mesh, ctx->material, ctx->instances, sizeof(text_instance_t), ctx->instance_count);
}
