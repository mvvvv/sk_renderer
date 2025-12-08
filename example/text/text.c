// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Nick Klingensmith

#include "text.h"
#include "text_internal.h"
#include "../tools/scene_util.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

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
	skr_buffer_t band_buffer;   // Bands for all glyphs (TEXT_BAND_COUNT per glyph)
	skr_buffer_t glyph_buffer;  // Per-glyph metadata

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
// curves that intersect that band.
static void _organize_curves_into_bands(
	text_curve_t* glyph_curves,    // Curves for this glyph (from all_curves array)
	int32_t       curve_count,
	float         glyph_y_min,
	float         glyph_y_max,
	text_array_t* out_bands,       // Array of text_band_t
	text_array_t* out_band_curves  // Re-ordered curves for band lookup
) {
	if (curve_count == 0) {
		// Empty glyph - add empty bands
		for (int32_t b = 0; b < TEXT_BAND_COUNT; b++) {
			text_band_t* band = _text_array_push(out_bands);
			band->curve_start = (uint32_t)out_band_curves->count;
			band->curve_count = 0;
		}
		return;
	}

	float glyph_height = glyph_y_max - glyph_y_min;
	if (glyph_height < 1e-6f) glyph_height = 1.0f;
	float band_height = glyph_height / TEXT_BAND_COUNT;

	// For each band, find curves that intersect it
	for (int32_t b = 0; b < TEXT_BAND_COUNT; b++) {
		float band_y_min = glyph_y_min + b * band_height;
		float band_y_max = glyph_y_min + (b + 1) * band_height;

		text_band_t* band = _text_array_push(out_bands);
		band->curve_start = (uint32_t)out_band_curves->count;
		band->curve_count = 0;

		// Check each curve for intersection with this band
		for (int32_t c = 0; c < curve_count; c++) {
			text_curve_t* curve = &glyph_curves[c];

			// Curve intersects band if their Y ranges overlap
			if (curve->y_max >= band_y_min && curve->y_min <= band_y_max) {
				// Copy curve to band's curve list
				text_curve_t* band_curve = _text_array_push(out_band_curves);
				*band_curve = *curve;
				band->curve_count++;
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// Curve Extraction
///////////////////////////////////////////////////////////////////////////////

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
	stbtt_vertex* vertices = NULL;
	int32_t       num_vertices = stbtt_GetGlyphShape(font, glyph_index, &vertices);

	*out_min_y =  1e10f;
	*out_max_y = -1e10f;

	if (num_vertices == 0 || vertices == NULL) {
		return;
	}

	float cx = 0, cy = 0;  // Current position

	for (int32_t i = 0; i < num_vertices; i++) {
		stbtt_vertex* v = &vertices[i];
		float x  = v->x * scale;
		float y  = v->y * scale;
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

				// Compute Y bounds
				float y_min = fminf(cy, y);
				float y_max = fmaxf(cy, y);
				curve->y_min = y_min;
				curve->y_max = y_max;

				*out_min_y = fminf(*out_min_y, y_min);
				*out_max_y = fmaxf(*out_max_y, y_max);

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

				// Compute Y bounds (include control point for conservative bounds)
				float y_min = fminf(fminf(cy, y), cy1);
				float y_max = fmaxf(fmaxf(cy, y), cy1);
				curve->y_min = y_min;
				curve->y_max = y_max;

				*out_min_y = fminf(*out_min_y, y_min);
				*out_max_y = fmaxf(*out_max_y, y_max);

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

				float y_min = fminf(fminf(fminf(cy, y), cy1), cy2);
				float y_max = fmaxf(fmaxf(fmaxf(cy, y), cy1), cy2);
				curve->y_min = y_min;
				curve->y_max = y_max;

				*out_min_y = fminf(*out_min_y, y_min);
				*out_max_y = fmaxf(*out_max_y, y_max);

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

text_font_t* text_font_load(const char* filename) {
	text_font_t* font = calloc(1, sizeof(text_font_t));
	if (!font) return NULL;

	// Load TTF file
	void*  ttf_data;
	size_t ttf_size;
	if (!su_file_read(filename, &ttf_data, &ttf_size)) {
		su_log(su_log_warning, "text: Failed to load font file: %s", filename);
		free(font);
		return NULL;
	}
	font->ttf_data = ttf_data;

	// Initialize stb_truetype
	if (!stbtt_InitFont(&font->stb_font, ttf_data, 0)) {
		su_log(su_log_warning, "text: Failed to parse font: %s", filename);
		free(ttf_data);
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

	// Temporary arrays for collecting curves and bands
	text_array_t temp_curves;   // Per-glyph curves (temporary, before band organization)
	text_array_t all_bands;     // All bands for all glyphs (TEXT_BAND_COUNT per glyph)
	text_array_t band_curves;   // Band-organized curves (final, uploaded to GPU)

	_text_array_init(&temp_curves, sizeof(text_curve_t));
	_text_array_init(&all_bands,   sizeof(text_band_t));
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
		_extract_glyph_curves(&font->stb_font, glyph->stb_glyph, scale,
		                      &temp_curves, &glyph_min_y, &glyph_max_y);

		glyph->gpu.curve_count = (uint32_t)temp_curves.count;
		glyph->gpu.curve_start = (uint32_t)band_curves.count;  // Points to band-organized curves

		// Record band start index for this glyph
		glyph->gpu.band_start = (uint32_t)all_bands.count;

		// Organize this glyph's curves into bands
		_organize_curves_into_bands(
			(text_curve_t*)temp_curves.data,
			temp_curves.count,
			glyph->gpu.bounds_min[1],  // Use bounding box Y for bands
			glyph->gpu.bounds_max[1],
			&all_bands,
			&band_curves
		);
	}

	// Upload to GPU buffers
	if (band_curves.count > 0) {
		skr_buffer_create(band_curves.data, band_curves.count, sizeof(text_curve_t),
		                  skr_buffer_type_storage, skr_use_static, &font->curve_buffer);
		skr_buffer_set_name(&font->curve_buffer, "text_curves");
	}

	// Upload bands
	if (all_bands.count > 0) {
		skr_buffer_create(all_bands.data, all_bands.count, sizeof(text_band_t),
		                  skr_buffer_type_storage, skr_use_static, &font->band_buffer);
		skr_buffer_set_name(&font->band_buffer, "text_bands");
	}

	// Upload glyph GPU data
	text_glyph_gpu_t glyph_gpu_data[TEXT_ASCII_COUNT];
	for (int32_t i = 0; i < TEXT_ASCII_COUNT; i++) {
		glyph_gpu_data[i] = font->glyphs[i].gpu;
	}
	skr_buffer_create(glyph_gpu_data, TEXT_ASCII_COUNT, sizeof(text_glyph_gpu_t),
	                  skr_buffer_type_storage, skr_use_static, &font->glyph_buffer);
	skr_buffer_set_name(&font->glyph_buffer, "text_glyphs");

	int32_t total_curves = band_curves.count;
	int32_t total_bands  = all_bands.count;
	_text_array_free(&temp_curves);
	_text_array_free(&all_bands);
	_text_array_free(&band_curves);

	font->valid = true;
	su_log(su_log_info, "text: Loaded font %s (%d curves, %d bands)", filename, total_curves, total_bands);

	return font;
}

void text_font_destroy(text_font_t* font) {
	if (!font) return;

	skr_buffer_destroy(&font->curve_buffer);
	skr_buffer_destroy(&font->band_buffer);
	skr_buffer_destroy(&font->glyph_buffer);

	if (font->ttf_data) {
		free(font->ttf_data);
	}

	free(font);
}

bool text_font_is_valid(text_font_t* font) {
	return font && font->valid;
}

float text_font_get_ascent(text_font_t* font) {
	return font ? font->ascent : 0;
}

float text_font_get_descent(text_font_t* font) {
	return font ? font->descent : 0;
}

float text_font_get_line_gap(text_font_t* font) {
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

	skr_mesh_create(&ctx->quad_vertex_type, skr_index_fmt_u16,
	                quad_verts, 4, quad_indices, 6, &ctx->quad_mesh);
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

float text_measure_width(text_font_t* font, const char* text) {
	if (!font || !text) return 0;

	float width = 0;
	const char* p = text;
	int32_t prev_codepoint = 0;

	while (*p) {
		int32_t codepoint = (int32_t)(unsigned char)*p;
		if (codepoint < TEXT_ASCII_START || codepoint >= TEXT_ASCII_END) {
			p++;
			continue;
		}

		int32_t idx = codepoint - TEXT_ASCII_START;
		text_glyph_t* glyph = &font->glyphs[idx];

		// Add kerning
		if (prev_codepoint) {
			int32_t kern = stbtt_GetCodepointKernAdvance(&font->stb_font, prev_codepoint, codepoint);
			width += kern / font->units_per_em;
		}

		width += glyph->gpu.advance;
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

	text_font_t* font = ctx->font;
	float scale = size;  // Size is in world units, font is normalized

	// Measure text width for alignment
	float text_width = text_measure_width(font, text) * scale;
	float offset_x = 0;

	switch (align) {
	case text_align_left:   offset_x = 0;                break;
	case text_align_center: offset_x = -text_width / 2;  break;
	case text_align_right:  offset_x = -text_width;      break;
	}

	// Process each character
	float cursor_x = offset_x;
	const char* p = text;
	int32_t prev_codepoint = 0;

	while (*p && ctx->instance_count < TEXT_MAX_INSTANCES) {
		int32_t codepoint = (int32_t)(unsigned char)*p;
		if (codepoint < TEXT_ASCII_START || codepoint >= TEXT_ASCII_END) {
			p++;
			continue;
		}

		int32_t idx = codepoint - TEXT_ASCII_START;
		text_glyph_t* glyph = &font->glyphs[idx];

		// Add kerning
		if (prev_codepoint) {
			int32_t kern = stbtt_GetCodepointKernAdvance(&font->stb_font, prev_codepoint, codepoint);
			cursor_x += (kern / font->units_per_em) * scale;
		}

		// Skip glyphs with no visual (like space)
		if (glyph->gpu.curve_count > 0) {
			// Build instance transform
			// Translate to cursor position, then scale by font size
			float4x4 char_transform = transform;

			// Apply cursor offset and scale
			// float4x4_mul(A, B) produces matrix that applies B first, then A
			// We want: local first (scale + cursor offset), then world transform
			float4x4 local = float4x4_trs(
				(float3){ cursor_x, 0, 0 },
				(float4){ 0, 0, 0, 1 },  // Identity quaternion
				(float3){ scale, scale, 1 }
			);
			char_transform = float4x4_mul(transform, local);

			// Add instance
			text_instance_t* inst = &ctx->instances[ctx->instance_count++];
			memcpy(inst->transform, &char_transform.m[0], sizeof(float) * 16);
			inst->glyph_index = (uint32_t)idx;
			inst->_pad0       = 0;
			inst->_pad1       = 0;
			inst->_pad2       = 0;
			inst->color[0]    = color.x;
			inst->color[1]    = color.y;
			inst->color[2]    = color.z;
			inst->color[3]    = color.w;
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
	skr_material_set_buffer(ctx->material, "bands",  &ctx->font->band_buffer);
	skr_material_set_buffer(ctx->material, "glyphs", &ctx->font->glyph_buffer);

	// Add to render list with instance data (automatic instancing via t2/space0)
	skr_render_list_add(render_list, &ctx->quad_mesh, ctx->material,
	                    ctx->instances, sizeof(text_instance_t), ctx->instance_count);
}
