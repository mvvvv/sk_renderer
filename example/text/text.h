// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Nick Klingensmith

// GPU-evaluated vector text rendering for sk_renderer.
//
// This module provides resolution-independent text rendering by evaluating
// glyph contours (quadratic Bezier curves) directly in the fragment shader.
// Text can be rendered in 3D space with perfect quality at any scale or angle.
//
// Usage:
//   // Load TTF file into memory (use your own file I/O)
//   void* ttf_data; size_t ttf_size;
//   load_file("CascadiaMono.ttf", &ttf_data, &ttf_size);
//
//   text_font_t*    font = text_font_load(ttf_data, ttf_size);
//   free(ttf_data);  // Font copies data internally
//   text_context_t* ctx  = text_context_create(font, &shader, &material);
//
//   // Each frame:
//   text_context_clear(ctx);
//   text_add(ctx, "Hello!", transform, 0.5f, color, text_align_left);
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

// Load a TrueType font from memory and extract glyph curves for GPU rendering.
// The data is copied internally, so the caller can free it after this returns.
// Returns NULL on failure.
text_font_t* text_font_load    (const void* ttf_data, size_t ttf_size);
void         text_font_destroy (      text_font_t* font); // Destroy a font and release all GPU resources.
bool         text_font_is_valid(const text_font_t* font); // Check if font is valid (successfully loaded).

///////////////////////////////////////////////////////////////////////////////
// Text Context
///////////////////////////////////////////////////////////////////////////////

// Create a text rendering context.
// The context holds per-frame instance data and rendering resources.
// shader and material are the text rendering shader/material (created externally).
text_context_t* text_context_create (text_font_t* font, skr_shader_t* shader, skr_material_t* material);
void            text_context_destroy(text_context_t* ctx);// Destroy a text context and release resources.
void            text_context_clear  (text_context_t* ctx);// Clear all text instances (call at start of each frame).

///////////////////////////////////////////////////////////////////////////////
// Text Rendering
///////////////////////////////////////////////////////////////////////////////

// Add text to be rendered this frame.
//
// Parameters:
//   ctx       - Text context
//   text      - ASCII text string to render (NULL-terminated)
//   transform - World transform for the text origin (baseline left)
//   size      - Font size in world units (height of capital letters)
//   color     - RGBA color (alpha used for blending)
//   align     - Text alignment relative to transform origin
void text_add(
	text_context_t* ctx,
	const char*     text,
	float4x4        transform,
	float           size,
	float4          color,
	text_align_t    align
);

// Submit all accumulated text to the render list.
// This uploads instance data to GPU and adds draw commands.
void text_render(text_context_t* ctx, skr_render_list_t* render_list);

///////////////////////////////////////////////////////////////////////////////
// Font Metrics (for advanced layout)
///////////////////////////////////////////////////////////////////////////////


float text_font_get_ascent  (const text_font_t* font);// Get the ascender height in normalized units (typically ~0.8-1.0)
float text_font_get_descent (const text_font_t* font);// Get the descender depth in normalized units (typically ~-0.2)
float text_font_get_line_gap(const text_font_t* font);// Get the line gap in normalized units
float text_measure_width    (const text_font_t* font, const char* text); // Measure the width of a text string in normalized units. Multiply by size to get world units.

#ifdef __cplusplus
}
#endif
