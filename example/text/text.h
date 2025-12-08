// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Nick Klingensmith

// GPU-evaluated vector text rendering for sk_renderer.
//
// This module provides resolution-independent text rendering by evaluating
// glyph contours (quadratic Bezier curves) directly in the fragment shader.
// Text can be rendered in 3D space with perfect quality at any scale or angle.
//
// Supports full Unicode via UTF-8 and UTF-16 encodings. Glyphs are loaded
// lazily on first use, so startup is fast and memory is only used for
// characters that are actually rendered.
//
// Usage:
//   // Load TTF file into memory (use your own file I/O)
//   void* ttf_data; size_t ttf_size;
//   load_file("NotoSans-Regular.ttf", &ttf_data, &ttf_size);
//
//   text_font_t*    font = text_font_load(ttf_data, ttf_size);
//   free(ttf_data);  // Font copies data internally
//   text_context_t* ctx  = text_context_create(font, &shader, &material);
//
//   // Each frame:
//   text_context_clear(ctx);
//   text_add(ctx, "Hello! Привет! 你好!", transform, 0.5f, color, text_align_left);
//   text_render(ctx, render_list);
//
//   // Cleanup:
//   text_context_destroy(ctx);
//   text_font_destroy(font);

#pragma once

#include <sk_renderer.h>
#include "../tools/float_math.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////
// Types
///////////////////////////////////////////////////////////////////////////////

typedef struct text_font_t    text_font_t;
typedef struct text_context_t text_context_t;

// Text alignment options
typedef enum {
	text_align_left,
	text_align_center,
	text_align_right
} text_align_t;

///////////////////////////////////////////////////////////////////////////////
// Font Management
///////////////////////////////////////////////////////////////////////////////

// Load a TrueType font from memory and prepare for GPU rendering.
// Glyphs are loaded lazily on first use, so this is fast.
// The data is copied internally, so the caller can free it after this returns.
// Returns NULL on failure.
text_font_t* text_font_load    (const void* ttf_data, size_t ttf_size);
void         text_font_destroy (      text_font_t* font);
bool         text_font_is_valid(const text_font_t* font);

///////////////////////////////////////////////////////////////////////////////
// Text Context
///////////////////////////////////////////////////////////////////////////////

// Create a text rendering context.
// The context holds per-frame instance data and rendering resources.
// shader and material are the text rendering shader/material (created externally).
text_context_t* text_context_create (text_font_t* font, skr_shader_t* shader, skr_material_t* material);
void            text_context_destroy(text_context_t* ctx);
void            text_context_clear  (text_context_t* ctx);

///////////////////////////////////////////////////////////////////////////////
// Text Rendering - UTF-8
///////////////////////////////////////////////////////////////////////////////

// Add UTF-8 encoded text to be rendered this frame.
//
// Parameters:
//   ctx       - Text context
//   text      - UTF-8 text string (NULL-terminated)
//   transform - World transform for the text origin (baseline left)
//   size      - Font size in world units (height of capital letters)
//   color     - RGBA color (alpha used for blending)
//   align     - Text alignment relative to transform origin
void text_add_utf8(
	text_context_t* ctx,
	const char*     text,
	float4x4        transform,
	float           size,
	float4          color,
	text_align_t    align
);

// Measure the width of a UTF-8 string in normalized units.
// Multiply by size to get world units.
float text_measure_width_utf8(text_font_t* font, const char* text);

///////////////////////////////////////////////////////////////////////////////
// Text Rendering - UTF-16
///////////////////////////////////////////////////////////////////////////////

// Add UTF-16 encoded text to be rendered this frame.
// Useful for Windows wchar_t strings or other UTF-16 sources.
void text_add_utf16(
	text_context_t*  ctx,
	const uint16_t*  text,
	float4x4         transform,
	float            size,
	float4           color,
	text_align_t     align
);

// Measure the width of a UTF-16 string in normalized units.
float text_measure_width_utf16(text_font_t* font, const uint16_t* text);

///////////////////////////////////////////////////////////////////////////////
// Backward Compatible Aliases
///////////////////////////////////////////////////////////////////////////////

// text_add() is an alias for text_add_utf8()
void text_add(
	text_context_t* ctx,
	const char*     text,
	float4x4        transform,
	float           size,
	float4          color,
	text_align_t    align
);

// text_measure_width() is an alias for text_measure_width_utf8()
float text_measure_width(const text_font_t* font, const char* text);

///////////////////////////////////////////////////////////////////////////////
// Rendering
///////////////////////////////////////////////////////////////////////////////

// Submit all accumulated text to the render list.
// This uploads instance data to GPU and adds draw commands.
void text_render(text_context_t* ctx, skr_render_list_t* render_list);

///////////////////////////////////////////////////////////////////////////////
// Font Metrics (for advanced layout)
///////////////////////////////////////////////////////////////////////////////

float text_font_get_ascent  (const text_font_t* font); // Ascender height (~0.8-1.0)
float text_font_get_descent (const text_font_t* font); // Descender depth (~-0.2)
float text_font_get_line_gap(const text_font_t* font); // Line gap

#ifdef __cplusplus
}
#endif
