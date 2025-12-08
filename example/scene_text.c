// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Nick Klingensmith

#include "scene.h"
#include "tools/scene_util.h"
#include "app.h"
#include "text/text.h"

#include "tools/float_math.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

// Vector text rendering demo scene
// Demonstrates GPU-evaluated text rendering in 3D
typedef struct {
	scene_t         base;

	text_font_t*    font;
	text_context_t* text_ctx;
	skr_shader_t    text_shader;
	skr_material_t  text_material;

	float           time;
	float           rotation_speed;
	float           font_size;
	bool            enable_rotation;
	int32_t         align_mode;   // 0=left, 1=center, 2=right
	char*           font_path;    // Current font path (for display)

	// Camera state (arc-ball style)
	float           cam_yaw;       // Horizontal angle (radians)
	float           cam_pitch;     // Vertical angle (radians)
	float           cam_distance;  // Distance from target
	float3          cam_target;    // Look-at target point
	// Velocities for smooth motion
	float           cam_yaw_vel;
	float           cam_pitch_vel;
	float           cam_distance_vel;
	float3          cam_target_vel;
} scene_text_t;

// Helper to get just the filename from a path
static const char* _get_filename(const char* path) {
	if (!path) return "(none)";
	const char* last_slash = strrchr(path, '/');
	if (!last_slash) last_slash = strrchr(path, '\\');
	return last_slash ? last_slash + 1 : path;
}

// Helper to load a font from a file path
static text_font_t* _load_font_file(const char* path) {
	void*  data;
	size_t size;
	if (!su_file_read(path, &data, &size)) {
		su_log(su_log_warning, "scene_text: Failed to read font file: %s", path);
		return NULL;
	}
	text_font_t* font = text_font_load(data, size);
	free(data);
	if (!text_font_is_valid(font)) {
		su_log(su_log_warning, "scene_text: Failed to parse font: %s", path);
		text_font_destroy(font);
		return NULL;
	}
	su_log(su_log_info, "scene_text: Loaded font: %s", _get_filename(path));
	return font;
}

// Helper to reload font from a path
static void _reload_font_from_path(scene_text_t* scene, const char* path) {
	text_font_t* new_font = _load_font_file(path);
	if (!new_font) return;

	// Destroy old context and font
	text_context_destroy(scene->text_ctx);
	text_font_destroy(scene->font);

	// Set up new font
	scene->font = new_font;
	if (scene->font_path) free(scene->font_path);
	scene->font_path = strdup(path);
	scene->text_ctx = text_context_create(scene->font, &scene->text_shader, &scene->text_material);
}

static scene_t* _scene_text_create(void) {
	scene_text_t* scene = calloc(1, sizeof(scene_text_t));
	if (!scene) return NULL;

	scene->base.size       = sizeof(scene_text_t);
	scene->time            = 0.0f;
	scene->rotation_speed  = 0.3f;
	scene->font_size       = 2.0f;
	scene->enable_rotation = false;
	scene->align_mode      = 1;  // Center by default
	scene->font_path       = strdup("CascadiaMono.ttf");

	// Initialize camera
	scene->cam_yaw          = 0.0f;
	scene->cam_pitch        = 0.0f;
	scene->cam_distance     = 10.0f;
	scene->cam_target       = (float3){ 0.0f, -1.0f, -4.0f };
	scene->cam_yaw_vel      = 0.0f;
	scene->cam_pitch_vel    = 0.0f;
	scene->cam_distance_vel = 0.0f;
	scene->cam_target_vel   = (float3){ 0.0f, 0.0f, 0.0f };

	// Load font
	scene->font = _load_font_file("CascadiaMono.ttf");
	if (!scene->font) {
		free(scene->font_path);
		free(scene);
		return NULL;
	}

	// Load text shader
	scene->text_shader = su_shader_load("shaders/text.hlsl.sks", "text_vector");
	if (!skr_shader_is_valid(&scene->text_shader)) {
		su_log(su_log_warning, "scene_text: Failed to load text shader");
		text_font_destroy(scene->font);
		free(scene);
		return NULL;
	}

	// Create material for text rendering
	skr_material_create((skr_material_info_t){
		.shader      = &scene->text_shader,
		.cull        = skr_cull_none,      // Double-sided for 3D viewing
		.depth_test  = skr_compare_less,
		.alpha_to_coverage = true,    // For anti-aliased edges
	}, &scene->text_material);

	// Create text context
	scene->text_ctx = text_context_create(scene->font, &scene->text_shader, &scene->text_material);
	if (!scene->text_ctx) {
		su_log(su_log_warning, "scene_text: Failed to create text context");
		skr_material_destroy(&scene->text_material);
		skr_shader_destroy(&scene->text_shader);
		text_font_destroy(scene->font);
		free(scene);
		return NULL;
	}

	su_log(su_log_info, "scene_text: Vector text scene initialized");
	return (scene_t*)scene;
}

static void _scene_text_destroy(scene_t* base) {
	scene_text_t* scene = (scene_text_t*)base;

	text_context_destroy(scene->text_ctx);
	text_font_destroy(scene->font);
	skr_material_destroy(&scene->text_material);
	skr_shader_destroy(&scene->text_shader);
	if (scene->font_path) free(scene->font_path);

	free(scene);
}

static void _scene_text_update(scene_t* base, float delta_time) {
	scene_text_t* scene = (scene_text_t*)base;
	scene->time += delta_time;

	// Camera control constants
	const float rotate_sensitivity = 0.0002f;
	const float pan_sensitivity    = 0.0001f;
	const float zoom_sensitivity   = 0.2f;
	const float velocity_damping   = 0.0001f;  // Per-second retention (lower = more damping)
	const float pitch_limit        = 1.5f;     // ~86 degrees
	const float min_distance       = 1.0f;
	const float max_distance       = 50.0f;

	// Get ImGui IO for mouse state
	ImGuiIO* io = igGetIO();

	// Only process input if ImGui doesn't want the mouse
	if (!io->WantCaptureMouse) {
		// Left mouse: arc rotate
		if (io->MouseDown[0]) {
			scene->cam_yaw_vel   -= io->MouseDelta.x * rotate_sensitivity;
			scene->cam_pitch_vel += io->MouseDelta.y * rotate_sensitivity;
		}

		// Right mouse: pan
		if (io->MouseDown[1]) {
			// Calculate camera right vector for panning (perpendicular to view direction)
			float cos_yaw = cosf(scene->cam_yaw);
			float sin_yaw = sinf(scene->cam_yaw);

			float3 right = { cos_yaw, 0.0f, -sin_yaw };

			float pan_scale = scene->cam_distance * pan_sensitivity;
			scene->cam_target_vel.x -= right.x * io->MouseDelta.x * pan_scale;
			scene->cam_target_vel.z -= right.z * io->MouseDelta.x * pan_scale;
			scene->cam_target_vel.y += io->MouseDelta.y * pan_scale;
		}

		// Scroll wheel: zoom
		if (io->MouseWheel != 0.0f) {
			scene->cam_distance_vel -= io->MouseWheel * zoom_sensitivity;
		}
	}

	// Apply velocities
	scene->cam_yaw      += scene->cam_yaw_vel;
	scene->cam_pitch    += scene->cam_pitch_vel;
	scene->cam_distance += scene->cam_distance_vel;
	scene->cam_target.x += scene->cam_target_vel.x;
	scene->cam_target.y += scene->cam_target_vel.y;
	scene->cam_target.z += scene->cam_target_vel.z;

	// Clamp pitch and distance
	if (scene->cam_pitch >  pitch_limit) scene->cam_pitch =  pitch_limit;
	if (scene->cam_pitch < -pitch_limit) scene->cam_pitch = -pitch_limit;
	if (scene->cam_distance < min_distance) scene->cam_distance = min_distance;
	if (scene->cam_distance > max_distance) scene->cam_distance = max_distance;

	// Apply damping (exponential decay)
	float damping = powf(velocity_damping, delta_time);
	scene->cam_yaw_vel      *= damping;
	scene->cam_pitch_vel    *= damping;
	scene->cam_distance_vel *= damping;
	scene->cam_target_vel.x *= damping;
	scene->cam_target_vel.y *= damping;
	scene->cam_target_vel.z *= damping;
}

static void _scene_text_render(scene_t* base, int32_t width, int32_t height,
                                skr_render_list_t* ref_render_list,
                                su_system_buffer_t* ref_system_buffer) {
	scene_text_t* scene = (scene_text_t*)base;

	// Clear previous frame's text
	text_context_clear(scene->text_ctx);

	// Get alignment from UI setting
	text_align_t align = (text_align_t)scene->align_mode;

	// Calculate rotation
	float rot_angle = scene->enable_rotation ? scene->time * scene->rotation_speed : 0;
	float4 rotation = float4_quat_from_euler((float3){ 0, rot_angle, 0 });

	// Main title - large rotating text
	{
		float4x4 transform = float4x4_trs(
			(float3){ 0, 1.5f, -4 },
			rotation,
			(float3){ 1, 1, 1 }
		);
		text_add(scene->text_ctx, "Vector Text",
		         transform, scene->font_size * 1.5f,
		         (float4){ 1.0f, 1.0f, 1.0f, 1.0f }, align);
	}

	// Subtitle
	{
		float4x4 transform = float4x4_trs(
			(float3){ 0, 0.7f, -4 },
			rotation,
			(float3){ 1, 1, 1 }
		);
		text_add(scene->text_ctx, "GPU Curve Evaluation",
		         transform, scene->font_size * 0.7f,
		         (float4){ 0.7f, 0.9f, 1.0f, 1.0f }, align);
	}

	// Demo text at different sizes - including Unicode!
	const char* demo_texts[] = {
		"Resolution Independent",
		"Привет мир! Cyrillic",      // Russian: "Hello world!"
		"日本語テキスト Chinese/Japanese", // "Japanese text"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ",
		"0123456789 αβγδ €£¥",       // Greek letters and currency symbols
	};
	int32_t num_demos = sizeof(demo_texts) / sizeof(demo_texts[0]);

	float y_offset = 0.0f;
	for (int32_t i = 0; i < num_demos; i++) {
		float size = scene->font_size * (0.6f - i * 0.08f);
		if (size < 0.1f) size = 0.1f;

		// Alternate colors
		float4 color = (i % 2 == 0)
			? (float4){ 0.9f, 0.9f, 0.7f, 1.0f }
			: (float4){ 0.7f, 0.9f, 0.8f, 1.0f };

		float4x4 transform = float4x4_trs(
			(float3){ 0, y_offset, -4 },
			rotation,
			(float3){ 1, 1, 1 }
		);

		text_add(scene->text_ctx, demo_texts[i], transform, size, color, align);
		y_offset -= size * 1.5f;
	}
	
	// Submit all text to render list
	text_render(scene->text_ctx, ref_render_list);
}

static void _scene_text_render_ui(scene_t* base) {
	scene_text_t* scene = (scene_text_t*)base;

	igText("Vector Text Settings");
	igSeparator();

	// Font display and picker
	igText("Font: %s", _get_filename(scene->font_path));
	if (su_file_dialog_supported()) {
		igSameLine(0, 5);
		if (igButton("Browse...", (ImVec2){0, 0})) {
			char* path = su_file_dialog_open("Select Font", "Font Files", "ttf;otf");
			if (path) {
				_reload_font_from_path(scene, path);
				free(path);
			}
		}
	}

	igSliderFloat("Font Size", &scene->font_size, 0.1f, 1.0f, "%.2f", 0);
	igSliderFloat("Rotation Speed", &scene->rotation_speed, 0.0f, 2.0f, "%.2f", 0);
	igCheckbox("Enable Rotation", &scene->enable_rotation);

	const char* align_names[] = { "Left", "Center", "Right" };
	igCombo_Str_arr("Alignment", &scene->align_mode, align_names, 3, 0);

	igSeparator();
	igText("Font Metrics:");
	igText("  Ascent:  %.3f", text_font_get_ascent(scene->font));
	igText("  Descent: %.3f", text_font_get_descent(scene->font));
	igText("  Line Gap: %.3f", text_font_get_line_gap(scene->font));

	igSeparator();
	if (igButton("Reset Camera", (ImVec2){0, 0})) {
		scene->cam_yaw          = 0.0f;
		scene->cam_pitch        = 0.0f;
		scene->cam_distance     = 10.0f;
		scene->cam_target       = (float3){ 0.0f, -1.0f, -4.0f };
		scene->cam_yaw_vel      = 0.0f;
		scene->cam_pitch_vel    = 0.0f;
		scene->cam_distance_vel = 0.0f;
		scene->cam_target_vel   = (float3){ 0.0f, 0.0f, 0.0f };
	}
}

static bool _scene_text_get_camera(scene_t* base, scene_camera_t* out_camera) {
	scene_text_t* scene = (scene_text_t*)base;

	// Calculate camera position from spherical coordinates
	float cos_pitch = cosf(scene->cam_pitch);
	float sin_pitch = sinf(scene->cam_pitch);
	float cos_yaw   = cosf(scene->cam_yaw);
	float sin_yaw   = sinf(scene->cam_yaw);

	out_camera->position = (float3){
		scene->cam_target.x + scene->cam_distance * cos_pitch * sin_yaw,
		scene->cam_target.y + scene->cam_distance * sin_pitch,
		scene->cam_target.z + scene->cam_distance * cos_pitch * cos_yaw
	};
	out_camera->target = scene->cam_target;
	out_camera->up     = (float3){ 0.0f, 1.0f, 0.0f };

	return true;
}

const scene_vtable_t scene_text_vtable = {
	.name       = "Vector Text",
	.create     = _scene_text_create,
	.destroy    = _scene_text_destroy,
	.update     = _scene_text_update,
	.render     = _scene_text_render,
	.get_camera = _scene_text_get_camera,
	.render_ui  = _scene_text_render_ui,
};
