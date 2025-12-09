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

// Extended alignment flags (can be combined with |)
// Controls how text aligns within its bounding box
typedef enum {
	text_align_x_left   = 1 << 0,  // On the X axis, start on the left
	text_align_y_top    = 1 << 1,  // On the Y axis, start at the top
	text_align_x_center = 1 << 2,  // On the X axis, center the item
	text_align_y_center = 1 << 3,  // On the Y axis, center the item
	text_align_x_right  = 1 << 4,  // On the X axis, start on the right
	text_align_y_bottom = 1 << 5,  // On the Y axis, start at the bottom

	// Common combinations
	text_align_top_left     = text_align_x_left   | text_align_y_top,
	text_align_top_center   = text_align_x_center | text_align_y_top,
	text_align_top_right    = text_align_x_right  | text_align_y_top,
	text_align_center_left  = text_align_x_left   | text_align_y_center,
	text_align_center_center= text_align_x_center | text_align_y_center,
	text_align_center_right = text_align_x_right  | text_align_y_center,
	text_align_bottom_left  = text_align_x_left   | text_align_y_bottom,
	text_align_bottom_center= text_align_x_center | text_align_y_bottom,
	text_align_bottom_right = text_align_x_right  | text_align_y_bottom,
} text_align2_t;

// Pivot point flags - where the transform origin is relative to the text box
// Same bit values as text_align2_t but with semantic naming for clarity
typedef enum {
	text_pivot_x_left   = 1 << 0,  // Origin at left edge
	text_pivot_y_top    = 1 << 1,  // Origin at top edge
	text_pivot_x_center = 1 << 2,  // Origin at horizontal center
	text_pivot_y_center = 1 << 3,  // Origin at vertical center
	text_pivot_x_right  = 1 << 4,  // Origin at right edge
	text_pivot_y_bottom = 1 << 5,  // Origin at bottom edge

	// Common combinations
	text_pivot_top_left     = text_pivot_x_left   | text_pivot_y_top,
	text_pivot_top_center   = text_pivot_x_center | text_pivot_y_top,
	text_pivot_top_right    = text_pivot_x_right  | text_pivot_y_top,
	text_pivot_center_left  = text_pivot_x_left   | text_pivot_y_center,
	text_pivot_center       = text_pivot_x_center | text_pivot_y_center,
	text_pivot_center_right = text_pivot_x_right  | text_pivot_y_center,
	text_pivot_bottom_left  = text_pivot_x_left   | text_pivot_y_bottom,
	text_pivot_bottom_center= text_pivot_x_center | text_pivot_y_bottom,
	text_pivot_bottom_right = text_pivot_x_right  | text_pivot_y_bottom,
} text_pivot_t;

// How text fits within a bounding box
typedef enum {
	text_fit_none     = 0,       // No special behavior
	text_fit_wrap     = 1 << 0,  // Wrap text to next line at box width
	text_fit_clip     = 1 << 1,  // Truncate text that exceeds box
	text_fit_squeeze  = 1 << 2,  // Scale down if too large (won't scale up)
	text_fit_exact    = 1 << 3,  // Scale to fit exactly (up or down)
	text_fit_overflow = 1 << 4,  // Ignore box, keep going
} text_fit_t;

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
// Advanced Text Layout - UTF-8
///////////////////////////////////////////////////////////////////////////////

// Add text within a bounding box with full layout control.
//
// Parameters:
//   ctx       - Text context
//   text      - UTF-8 text string (NULL-terminated)
//   transform - World transform for the text box origin
//   box_size  - Size of the bounding box in world units (width, height)
//   font_size - Base font size in world units (height of capital letters)
//   fit       - How text should fit within the box (wrap, clip, squeeze, etc.)
//   pivot     - Where the transform origin is relative to the box
//   align     - How text aligns within the box (X and Y)
//   offset    - Offset for scrolling text within the box
//   color     - RGBA color
//
// Returns: Actual size of the rendered text (useful for layout)
float2 text_add_in_utf8(
	text_context_t* ctx,
	const char*     text,
	float4x4        transform,
	float2          box_size,
	float           font_size,
	text_fit_t      fit,
	text_pivot_t    pivot,
	text_align2_t   align,
	float2          offset,
	float4          color
);

// Measure text size with wrapping support.
// Returns (width, height) in normalized units. Multiply by font size for world units.
float2 text_measure_utf8(text_font_t* font, const char* text, float wrap_width);

// text_add_in() is an alias for text_add_in_utf8()
float2 text_add_in(
	text_context_t* ctx,
	const char*     text,
	float4x4        transform,
	float2          box_size,
	float           font_size,
	text_fit_t      fit,
	text_pivot_t    pivot,
	text_align2_t   align,
	float2          offset,
	float4          color
);

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
