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

#define TEXT_BAND_COUNT      16    // Number of horizontal bands per glyph
#define TEXT_ASCII_START     32    // First ASCII character (space)
#define TEXT_ASCII_END       127   // Last ASCII character (DEL excluded)
#define TEXT_ASCII_COUNT     (TEXT_ASCII_END - TEXT_ASCII_START)
#define TEXT_MAX_INSTANCES   4096  // Max characters per text_render() call

///////////////////////////////////////////////////////////////////////////////
// GPU Buffer Structures (must match shader exactly)
///////////////////////////////////////////////////////////////////////////////

// Quadratic Bezier curve (3 control points)
// Curves are stored in glyph-local coordinates (normalized to units_per_em)
typedef struct {
	float p0[2];        // Start point
	float p1[2];        // Control point
	float p2[2];        // End point
	float x_min;        // Curve bounding box (precomputed for GPU)
	float x_max;
	float y_min;
	float y_max;
} text_curve_t;         // 40 bytes

// Horizontal band - references curves that cross this Y range
// Bands enable O(n/bands) curve testing instead of O(n) per pixel
typedef struct {
	uint32_t curve_start;   // Index into curve array
	uint32_t curve_count;   // Number of curves in this band
} text_band_t;              // 8 bytes (used during loading only)

// Per-glyph metadata stored in GPU buffer
// Band data is embedded to eliminate one level of indirection
typedef struct {
	uint32_t curve_start;   // Base index into curve array
	uint32_t curve_count;   // Total number of curves for this glyph
	float    bounds_min[2]; // Glyph bounding box min (glyph space)
	float    bounds_max[2]; // Glyph bounding box max (glyph space)
	float    advance;       // Horizontal advance width
	float    lsb;           // Left side bearing
	uint32_t bands[TEXT_BAND_COUNT]; // Packed (offset << 16) | count per band
} text_glyph_gpu_t;         // 96 bytes

// Per-character instance data (uploaded each frame)
// Must match HLSL Instance struct exactly
// Uses position/right/up vectors instead of full matrix (50% smaller!)
typedef struct {
	float    pos[3];        // World position - 12 bytes, offset 0
	uint32_t glyph_index;   // Index into glyph buffer - 4 bytes, offset 12
	float    right[3];      // X axis * scale - 12 bytes, offset 16
	uint32_t color;         // Packed RGBA8 (0xAABBGGRR) - 4 bytes, offset 28
	float    up[3];         // Y axis * scale - 12 bytes, offset 32
	uint32_t _pad;          // Padding - 4 bytes, offset 44
} text_instance_t;          // 48 bytes total

_Static_assert(sizeof(text_instance_t) == 48, "text_instance_t must be exactly 48 bytes to match HLSL");

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

///////////////////////////////////////////////////////////////////////////////
// Font Structure
///////////////////////////////////////////////////////////////////////////////

struct text_font_t {
	// stb_truetype data
	stbtt_fontinfo stb_font;
	void*          ttf_data;

	// Font metrics (normalized to 1.0 = units_per_em)
	float units_per_em;
	float ascent;
	float descent;
	float line_gap;

	// CPU glyph data (for layout)
	text_glyph_t glyphs[TEXT_ASCII_COUNT];

	// GPU buffers
	skr_buffer_t curve_buffer;  // All curves for all glyphs
	skr_buffer_t glyph_buffer;  // Per-glyph metadata including band info

	bool valid;
};

///////////////////////////////////////////////////////////////////////////////
// Context Structure
///////////////////////////////////////////////////////////////////////////////

struct text_context_t {
	text_font_t*    font;
	skr_shader_t*   shader;
	skr_material_t* material;

	// Instance data for current frame (passed to render list)
	text_instance_t instances[TEXT_MAX_INSTANCES];
	int32_t         instance_count;

	// Quad mesh for rendering characters
	skr_mesh_t      quad_mesh;
	skr_vert_type_t quad_vertex_type;
};

///////////////////////////////////////////////////////////////////////////////
// Band Organization
///////////////////////////////////////////////////////////////////////////////

// Organize curves into horizontal bands for faster fragment shader lookup.
// Each band covers a portion of the glyph's Y range and references only
// curves that intersect that band. Band data is written directly to glyph.
static void _organize_curves_into_bands(
	text_curve_t*     glyph_curves,    // Curves for this glyph (temp array)
	int32_t           curve_count,
	float             glyph_y_min,
	float             glyph_y_max,
	text_glyph_gpu_t* out_glyph,       // Glyph to write band data to
	text_array_t*     out_band_curves  // Re-ordered curves for band lookup
) {
	// Record glyph's curve_start (base index for band offsets)
	uint32_t glyph_curve_start = (uint32_t)out_band_curves->count;
	out_glyph->curve_start = glyph_curve_start;

	if (curve_count == 0) {
		// Empty glyph - all bands are empty (offset=0, count=0)
		for (int32_t b = 0; b < TEXT_BAND_COUNT; b++) {
			out_glyph->bands[b] = 0;  // (0 << 16) | 0
		}
		out_glyph->curve_count = 0;
		return;
	}

	float glyph_height = glyph_y_max - glyph_y_min;
	if (glyph_height < 1e-6f) glyph_height = 1.0f;
	float band_height = glyph_height / TEXT_BAND_COUNT;

	// For each band, find curves that intersect it
	for (int32_t b = 0; b < TEXT_BAND_COUNT; b++) {
		float band_y_min = glyph_y_min + b * band_height;
		float band_y_max = glyph_y_min + (b + 1) * band_height;

		// Offset is relative to glyph's curve_start
		uint32_t band_offset = (uint32_t)out_band_curves->count - glyph_curve_start;
		uint32_t band_count  = 0;

		// Check each curve for intersection with this band
		for (int32_t c = 0; c < curve_count; c++) {
			text_curve_t* curve = &glyph_curves[c];

			// Curve intersects band if their Y ranges overlap
			if (curve->y_max >= band_y_min && curve->y_min <= band_y_max) {
				// Copy curve to band's curve list
				text_curve_t* band_curve = _text_array_push(out_band_curves);
				*band_curve = *curve;
				band_count++;
			}
		}

		// Pack offset and count into single uint32: (offset << 16) | count
		out_glyph->bands[b] = (band_offset << 16) | (band_count & 0xFFFF);
	}

	// Total curves for this glyph (in band-organized form)
	out_glyph->curve_count = (uint32_t)out_band_curves->count - glyph_curve_start;
}

///////////////////////////////////////////////////////////////////////////////
// Curve Extraction
///////////////////////////////////////////////////////////////////////////////

// Compute tight AABB for a quadratic Bezier curve (IQ's method)
// https://iquilezles.org/articles/bezierbbox/
static void _compute_curve_aabb(
	float p0x, float p0y,
	float p1x, float p1y,
	float p2x, float p2y,
	float* out_x_min, float* out_x_max,
	float* out_y_min, float* out_y_max
) {
	float ax = p0x - 2.0f * p1x + p2x;
	float ay = p0y - 2.0f * p1y + p2y;
	float bx = p1x - p0x;
	float by = p1y - p0y;

	// Clamp t to [0,1] - handles degenerate cases naturally
	float tx = (fabsf(ax) > 1e-8f) ? fminf(fmaxf(-bx / ax, 0.0f), 1.0f) : 0.0f;
	float ty = (fabsf(ay) > 1e-8f) ? fminf(fmaxf(-by / ay, 0.0f), 1.0f) : 0.0f;

	float qx = p0x + tx * (2.0f * bx + tx * ax);
	float qy = p0y + ty * (2.0f * by + ty * ay);

	*out_x_min = fminf(fminf(p0x, p2x), qx);
	*out_x_max = fmaxf(fmaxf(p0x, p2x), qx);
	*out_y_min = fminf(fminf(p0y, p2y), qy);
	*out_y_max = fmaxf(fmaxf(p0y, p2y), qy);
}

// Extract quadratic Bezier curves from a glyph shape.
// stb_truetype provides shapes as move/line/curve commands.
static void _extract_glyph_curves(
	stbtt_fontinfo* font,
	int32_t         glyph_index,
	float           scale,
	text_array_t*   out_curves,
	float*          out_min_y,
	float*          out_max_y
) {
	stbtt_vertex* vertices     = NULL;
	int32_t       num_vertices = stbtt_GetGlyphShape(font, glyph_index, &vertices);

	*out_min_y =  1e10f;
	*out_max_y = -1e10f;

	if (num_vertices == 0 || vertices == NULL) {
		return;
	}

	float cx = 0, cy = 0;  // Current position

	for (int32_t i = 0; i < num_vertices; i++) {
		stbtt_vertex* v = &vertices[i];
		float x   = v->x  * scale;
		float y   = v->y  * scale;
		float cx1 = v->cx * scale;
		float cy1 = v->cy * scale;

		switch (v->type) {
		case STBTT_vmove:
			// Start new contour
			cx = x;
			cy = y;
			break;

		case STBTT_vline:
			// Line segment - convert to degenerate quadratic (control point at midpoint)
			{
				text_curve_t* curve = _text_array_push(out_curves);
				curve->p0[0] = cx;
				curve->p0[1] = cy;
				curve->p1[0] = (cx + x) * 0.5f;
				curve->p1[1] = (cy + y) * 0.5f;
				curve->p2[0] = x;
				curve->p2[1] = y;

				_compute_curve_aabb(
					curve->p0[0], curve->p0[1],
					curve->p1[0], curve->p1[1],
					curve->p2[0], curve->p2[1],
					&curve->x_min, &curve->x_max,
					&curve->y_min, &curve->y_max);

				*out_min_y = fminf(*out_min_y, curve->y_min);
				*out_max_y = fmaxf(*out_max_y, curve->y_max);

				cx = x;
				cy = y;
			}
			break;

		case STBTT_vcurve:
			// Quadratic Bezier curve
			{
				text_curve_t* curve = _text_array_push(out_curves);
				curve->p0[0] = cx;
				curve->p0[1] = cy;
				curve->p1[0] = cx1;
				curve->p1[1] = cy1;
				curve->p2[0] = x;
				curve->p2[1] = y;

				_compute_curve_aabb(
					curve->p0[0], curve->p0[1],
					curve->p1[0], curve->p1[1],
					curve->p2[0], curve->p2[1],
					&curve->x_min, &curve->x_max,
					&curve->y_min, &curve->y_max);

				*out_min_y = fminf(*out_min_y, curve->y_min);
				*out_max_y = fmaxf(*out_max_y, curve->y_max);

				cx = x;
				cy = y;
			}
			break;

		case STBTT_vcubic:
			// Cubic Bezier - approximate with quadratics
			// For simplicity, use midpoint approximation (one quadratic)
			{
				float cx2 = v->cx1 * scale;
				float cy2 = v->cy1 * scale;

				// Approximate cubic with single quadratic using midpoint of control points
				text_curve_t* curve = _text_array_push(out_curves);
				curve->p0[0] = cx;
				curve->p0[1] = cy;
				curve->p1[0] = (cx1 + cx2) * 0.5f;
				curve->p1[1] = (cy1 + cy2) * 0.5f;
				curve->p2[0] = x;
				curve->p2[1] = y;

				_compute_curve_aabb(
					curve->p0[0], curve->p0[1],
					curve->p1[0], curve->p1[1],
					curve->p2[0], curve->p2[1],
					&curve->x_min, &curve->x_max,
					&curve->y_min, &curve->y_max);

				*out_min_y = fminf(*out_min_y, curve->y_min);
				*out_max_y = fmaxf(*out_max_y, curve->y_max);

				cx = x;
				cy = y;
			}
			break;
		}
	}

	stbtt_FreeShape(font, vertices);
}

///////////////////////////////////////////////////////////////////////////////
// Font Loading
///////////////////////////////////////////////////////////////////////////////

text_font_t* text_font_load(const void* ttf_data, size_t ttf_size) {
	if (!ttf_data || ttf_size == 0) return NULL;

	text_font_t* font = calloc(1, sizeof(text_font_t));
	if (!font) return NULL;

	// Copy TTF data (stb_truetype needs it to remain valid)
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

	// Use ascent as the normalization factor (capital height ~ ascent)
	font->units_per_em = (float)ascent;
	float scale        = 1.0f / font->units_per_em;
	font->ascent       = ascent   * scale;
	font->descent      = descent  * scale;
	font->line_gap     = line_gap * scale;

	// Temporary arrays for collecting curves
	text_array_t temp_curves;   // Per-glyph curves (temporary, before band organization)
	text_array_t band_curves;   // Band-organized curves (final, uploaded to GPU)

	_text_array_init(&temp_curves, sizeof(text_curve_t));
	_text_array_init(&band_curves, sizeof(text_curve_t));

	// Process each ASCII character
	for (int32_t c = TEXT_ASCII_START; c < TEXT_ASCII_END; c++) {
		int32_t       idx   = c - TEXT_ASCII_START;
		text_glyph_t* glyph = &font->glyphs[idx];

		glyph->codepoint = c;
		glyph->stb_glyph = stbtt_FindGlyphIndex(&font->stb_font, c);

		// Get glyph metrics
		int32_t advance, lsb;
		stbtt_GetGlyphHMetrics(&font->stb_font, glyph->stb_glyph, &advance, &lsb);
		glyph->gpu.advance = advance * scale;
		glyph->gpu.lsb     = lsb * scale;

		// Get glyph bounding box (in font units, scale to normalized)
		int32_t x0, y0, x1, y1;
		stbtt_GetGlyphBox(&font->stb_font, glyph->stb_glyph, &x0, &y0, &x1, &y1);
		glyph->gpu.bounds_min[0] = x0 * scale;
		glyph->gpu.bounds_min[1] = y0 * scale;
		glyph->gpu.bounds_max[0] = x1 * scale;
		glyph->gpu.bounds_max[1] = y1 * scale;

		// Extract curves for this glyph into temp array
		temp_curves.count = 0;  // Reset for each glyph
		float glyph_min_y, glyph_max_y;
		_extract_glyph_curves(&font->stb_font, glyph->stb_glyph, scale, &temp_curves, &glyph_min_y, &glyph_max_y);

		// Organize curves into bands (writes curve_start, curve_count, bands[] to glyph->gpu)
		_organize_curves_into_bands(
			(text_curve_t*)temp_curves.data,
			temp_curves.count,
			glyph->gpu.bounds_min[1],  // Use bounding box Y for bands
			glyph->gpu.bounds_max[1],
			&glyph->gpu,
			&band_curves
		);
	}

	// Upload to GPU buffers
	if (band_curves.count > 0) {
		skr_buffer_create(band_curves.data, band_curves.count, sizeof(text_curve_t),
		                  skr_buffer_type_storage, skr_use_static, &font->curve_buffer);
		skr_buffer_set_name(&font->curve_buffer, "text_curves");
	}

	// Upload glyph GPU data (includes embedded band info)
	text_glyph_gpu_t glyph_gpu_data[TEXT_ASCII_COUNT];
	for (int32_t i = 0; i < TEXT_ASCII_COUNT; i++) {
		glyph_gpu_data[i] = font->glyphs[i].gpu;
	}
	skr_buffer_create  (glyph_gpu_data, TEXT_ASCII_COUNT, sizeof(text_glyph_gpu_t), skr_buffer_type_storage, skr_use_static, &font->glyph_buffer);
	skr_buffer_set_name(&font->glyph_buffer, "text_glyphs");

	_text_array_free(&temp_curves);
	_text_array_free(&band_curves);

	font->valid = true;
	return font;
}

void text_font_destroy(text_font_t* font) {
	if (!font) return;

	skr_buffer_destroy(&font->curve_buffer);
	skr_buffer_destroy(&font->glyph_buffer);

	if (font->ttf_data) {
		free(font->ttf_data);
	}

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

	// Create simple quad vertex type and mesh
	// The quad spans [0,1] in X and Y, scaled by glyph bounds in shader
	typedef struct {
		float position[2];
		float uv      [2];
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

	skr_mesh_create  (&ctx->quad_vertex_type, skr_index_fmt_u16, quad_verts, 4, quad_indices, 6, &ctx->quad_mesh);
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
// Text Layout & Rendering
///////////////////////////////////////////////////////////////////////////////

float text_measure_width(const text_font_t* font, const char* text) {
	if (!font || !text) return 0;

	float       width          = 0;
	const char* p              = text;
	int32_t     prev_codepoint = 0;

	while (*p) {
		int32_t codepoint = (int32_t)(unsigned char)*p;
		if (codepoint < TEXT_ASCII_START || codepoint >= TEXT_ASCII_END) {
			p++;
			continue;
		}

		int32_t             idx   = codepoint - TEXT_ASCII_START;
		const text_glyph_t* glyph = &font->glyphs[idx];

		// Add kerning
		if (prev_codepoint) {
			int32_t kern = stbtt_GetCodepointKernAdvance(&font->stb_font, prev_codepoint, codepoint);
			width += kern / font->units_per_em;
		}

		width         += glyph->gpu.advance;
		prev_codepoint = codepoint;
		p++;
	}

	return width;
}

void text_add(
	text_context_t* ctx,
	const char*     text,
	float4x4        transform,
	float           size,
	float4          color,
	text_align_t    align
) {
	if (!ctx || !text || ctx->instance_count >= TEXT_MAX_INSTANCES) return;

	text_font_t* font  = ctx->font;
	float        scale = size;  // Size is in world units, font is normalized

	// Measure text width for alignment
	float text_width = text_measure_width(font, text) * scale;
	float offset_x   = 0;

	switch (align) {
	case text_align_left:   offset_x = 0;                break;
	case text_align_center: offset_x = -text_width / 2;  break;
	case text_align_right:  offset_x = -text_width;      break;
	}

	// Process each character
	float       cursor_x       = offset_x;
	const char* p              = text;
	int32_t     prev_codepoint = 0;

	while (*p && ctx->instance_count < TEXT_MAX_INSTANCES) {
		int32_t codepoint = (int32_t)(unsigned char)*p;
		if (codepoint < TEXT_ASCII_START || codepoint >= TEXT_ASCII_END) {
			p++;
			continue;
		}

		int32_t       idx   = codepoint - TEXT_ASCII_START;
		text_glyph_t* glyph = &font->glyphs[idx];

		// Add kerning
		if (prev_codepoint) {
			int32_t kern = stbtt_GetCodepointKernAdvance(&font->stb_font, prev_codepoint, codepoint);
			cursor_x += (kern / font->units_per_em) * scale;
		}

		// Skip glyphs with no visual (like space)
		if (glyph->gpu.curve_count > 0) {
			// Build instance transform
			// Apply cursor offset and scale, then user's world transform
			float4x4 local = float4x4_trs(
				(float3){ cursor_x, 0, 0 },
				(float4){ 0, 0, 0, 1 },  // Identity quaternion
				(float3){ scale, scale, 1 }
			);
			float4x4 final = float4x4_mul(transform, local);

			// Extract position/right/up from row-major matrix:
			// Translation is in last column (m[3], m[7], m[11])
			// Right/up are columns 0 and 1
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

			inst->glyph_index = (uint32_t)idx;
			inst->_pad        = 0;

			// Pack color as RGBA8 (0xAABBGGRR)
			uint32_t r = (uint32_t)(color.x * 255.0f) & 0xFF;
			uint32_t g = (uint32_t)(color.y * 255.0f) & 0xFF;
			uint32_t b = (uint32_t)(color.z * 255.0f) & 0xFF;
			uint32_t a = (uint32_t)(color.w * 255.0f) & 0xFF;
			inst->color = r | (g << 8) | (b << 16) | (a << 24);
		}

		cursor_x += glyph->gpu.advance * scale;
		prev_codepoint = codepoint;
		p++;
	}
}

void text_render(text_context_t* ctx, skr_render_list_t* render_list) {
	if (!ctx || ctx->instance_count == 0) return;

	// Bind font data buffers to material
	skr_material_set_buffer(ctx->material, "curves", &ctx->font->curve_buffer);
	skr_material_set_buffer(ctx->material, "glyphs", &ctx->font->glyph_buffer);

	// Add to render list with instance data (automatic instancing via t2/space0)
	skr_render_list_add(render_list, &ctx->quad_mesh, ctx->material, ctx->instances, sizeof(text_instance_t), ctx->instance_count);
}
