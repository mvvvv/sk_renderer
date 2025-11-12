// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#pragma once

#include <sk_renderer.h>
#include <stdbool.h>
#include <stdint.h>

// Platform-agnostic application interface
// The app works entirely with sk_renderer abstractions and knows nothing about SDL, surfaces, or windowing

// Application-level system buffer for shaders
// Max 6 views for stereo/multi-view rendering
#define APP_MAX_VIEWS 6
typedef struct {
	float    view          [APP_MAX_VIEWS][16];  // View matrices (one per view)
	float    view_inv      [APP_MAX_VIEWS][16];  // Inverse view matrices
	float    projection    [APP_MAX_VIEWS][16];  // Per-view projection matrices
	float    projection_inv[APP_MAX_VIEWS][16];  // Inverse projection matrices
	float    viewproj      [APP_MAX_VIEWS][16];  // Precomputed view*projection matrices
	float    cam_pos       [APP_MAX_VIEWS][4];   // Camera position (xyz + padding)
	float    cam_dir       [APP_MAX_VIEWS][4];   // Camera forward direction (xyz + padding)
	float    cubemap_info  [4];                   // .xy = size, .z = mip count, .w = unused
	float    time;                                // Time in seconds
	uint32_t view_count;                          // Number of active views (1-6)
	uint32_t _pad[2];
} app_system_buffer_t;

// Application state (opaque to platform layer)
typedef struct app_t app_t;

// Keyboard input
typedef enum {
	app_key_left,
	app_key_right,
} app_key_;

// Lifecycle functions
app_t* app_create (void);
void   app_destroy(app_t* app);

// Input handling
void app_key_press(app_t* app, app_key_ key);

// Per-frame functions
void app_update(app_t* app, float delta_time);
void app_render(app_t* app, skr_tex_t* render_target, int32_t width, int32_t height);

// ImGui UI building (builds the UI, does NOT render)
void app_render_imgui(app_t* app, skr_tex_t* render_target, int32_t width, int32_t height);
