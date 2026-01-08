// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2026 Nick Klingensmith
// Copyright (c) 2026 Qualcomm Technologies, Inc.

#include "scene.h"
#include "tools/scene_util.h"
#include "app.h"

#include <stdlib.h>
#include <string.h>

#include <sk_app.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

// BC1 (DXT1) compression demo
// Demonstrates simple runtime BC1 compression with a fast min/max encoder

typedef struct {
	scene_t        base;
	skr_mesh_t     quad_mesh;
	skr_shader_t   shader;
	skr_material_t material_original;
	skr_material_t material_bc1;
	skr_tex_t      texture_original;
	skr_tex_t      texture_bc1;
	float          time;

	// Image info
	int32_t        img_width;
	int32_t        img_height;
	int32_t        bc1_size;

	// File loading UI
	char           file_path[512];
	bool           load_requested;

	// Camera
	float          cam_distance;
} scene_bc1_t;

///////////////////////////////////////////////////////////////////////////////
// Simple BC1 Encoder
///////////////////////////////////////////////////////////////////////////////

// Convert RGB888 to RGB565
static uint16_t _rgb888_to_565(uint8_t r, uint8_t g, uint8_t b) {
	return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Expand RGB565 back to RGB888 for comparison
static void _rgb565_to_888(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
	*r = ((c >> 11) & 0x1F) * 255 / 31;
	*g = ((c >> 5)  & 0x3F) * 255 / 63;
	*b = (c         & 0x1F) * 255 / 31;
}

// Simple squared distance between two RGB colors
static int32_t _color_dist_sq(uint8_t r0, uint8_t g0, uint8_t b0,
                              uint8_t r1, uint8_t g1, uint8_t b1) {
	int32_t dr = (int32_t)r0 - (int32_t)r1;
	int32_t dg = (int32_t)g0 - (int32_t)g1;
	int32_t db = (int32_t)b0 - (int32_t)b1;
	return dr*dr + dg*dg + db*db;
}

// Encode a single 4x4 block to BC1 (8 bytes output)
// Input: 16 pixels of RGBA8 (64 bytes)
// Output: 8 bytes BC1 data
static void _encode_bc1_block(const uint8_t* rgba, int32_t stride, uint8_t* out) {
	// Step 1: Find min/max RGB in the 4x4 block
	uint8_t min_r = 255, min_g = 255, min_b = 255;
	uint8_t max_r = 0,   max_g = 0,   max_b = 0;

	for (int32_t y = 0; y < 4; y++) {
		for (int32_t x = 0; x < 4; x++) {
			const uint8_t* p = rgba + y * stride + x * 4;
			if (p[0] < min_r) min_r = p[0];
			if (p[1] < min_g) min_g = p[1];
			if (p[2] < min_b) min_b = p[2];
			if (p[0] > max_r) max_r = p[0];
			if (p[1] > max_g) max_g = p[1];
			if (p[2] > max_b) max_b = p[2];
		}
	}

	// Step 2: Convert to RGB565
	uint16_t c0 = _rgb888_to_565(max_r, max_g, max_b);
	uint16_t c1 = _rgb888_to_565(min_r, min_g, min_b);

	// BC1 requires c0 > c1 for 4-color mode (no alpha)
	if (c0 < c1) {
		uint16_t tmp = c0; c0 = c1; c1 = tmp;
	}
	// Handle case where colors are equal
	if (c0 == c1 && c0 < 0xFFFF) {
		c0++;
	}

	// Step 3: Calculate the 4 interpolated colors
	uint8_t colors[4][3];
	_rgb565_to_888(c0, &colors[0][0], &colors[0][1], &colors[0][2]);
	_rgb565_to_888(c1, &colors[1][0], &colors[1][1], &colors[1][2]);

	// c2 = 2/3 * c0 + 1/3 * c1
	colors[2][0] = (2 * colors[0][0] + colors[1][0] + 1) / 3;
	colors[2][1] = (2 * colors[0][1] + colors[1][1] + 1) / 3;
	colors[2][2] = (2 * colors[0][2] + colors[1][2] + 1) / 3;

	// c3 = 1/3 * c0 + 2/3 * c1
	colors[3][0] = (colors[0][0] + 2 * colors[1][0] + 1) / 3;
	colors[3][1] = (colors[0][1] + 2 * colors[1][1] + 1) / 3;
	colors[3][2] = (colors[0][2] + 2 * colors[1][2] + 1) / 3;

	// Step 4: For each pixel, find best matching color index
	uint32_t indices = 0;
	for (int32_t y = 0; y < 4; y++) {
		for (int32_t x = 0; x < 4; x++) {
			const uint8_t* p = rgba + y * stride + x * 4;

			int32_t best_idx  = 0;
			int32_t best_dist = _color_dist_sq(p[0], p[1], p[2],
			                                   colors[0][0], colors[0][1], colors[0][2]);

			for (int32_t i = 1; i < 4; i++) {
				int32_t dist = _color_dist_sq(p[0], p[1], p[2],
				                              colors[i][0], colors[i][1], colors[i][2]);
				if (dist < best_dist) {
					best_dist = dist;
					best_idx  = i;
				}
			}

			// Pack 2-bit index (row-major, LSB first within each row)
			int32_t bit_pos = (y * 4 + x) * 2;
			indices |= (best_idx << bit_pos);
		}
	}

	// Step 5: Write output (little-endian)
	out[0] = c0 & 0xFF;
	out[1] = c0 >> 8;
	out[2] = c1 & 0xFF;
	out[3] = c1 >> 8;
	out[4] = (indices >>  0) & 0xFF;
	out[5] = (indices >>  8) & 0xFF;
	out[6] = (indices >> 16) & 0xFF;
	out[7] = (indices >> 24) & 0xFF;
}

// Compress an entire RGBA8 image to BC1
// Returns malloc'd BC1 data, caller must free
// Width and height should be multiples of 4
static uint8_t* _compress_to_bc1(const uint8_t* rgba, int32_t width, int32_t height) {
	int32_t blocks_x   = (width  + 3) / 4;
	int32_t blocks_y   = (height + 3) / 4;
	int32_t bc1_size   = blocks_x * blocks_y * 8;
	uint8_t* bc1_data  = malloc(bc1_size);
	int32_t stride     = width * 4;

	// Temporary buffer for edge blocks that extend past image bounds
	uint8_t block_rgba[4 * 4 * 4];

	for (int32_t by = 0; by < blocks_y; by++) {
		for (int32_t bx = 0; bx < blocks_x; bx++) {
			int32_t px = bx * 4;
			int32_t py = by * 4;

			const uint8_t* block_ptr;
			int32_t        block_stride;

			// Handle edge blocks by copying with clamping
			if (px + 4 > width || py + 4 > height) {
				for (int32_t y = 0; y < 4; y++) {
					for (int32_t x = 0; x < 4; x++) {
						int32_t sx = px + x < width  ? px + x : width  - 1;
						int32_t sy = py + y < height ? py + y : height - 1;
						const uint8_t* src = rgba + sy * stride + sx * 4;
						uint8_t*       dst = block_rgba + y * 16 + x * 4;
						dst[0] = src[0];
						dst[1] = src[1];
						dst[2] = src[2];
						dst[3] = src[3];
					}
				}
				block_ptr    = block_rgba;
				block_stride = 16;
			} else {
				block_ptr    = rgba + py * stride + px * 4;
				block_stride = stride;
			}

			uint8_t* out = bc1_data + (by * blocks_x + bx) * 8;
			_encode_bc1_block(block_ptr, block_stride, out);
		}
	}

	return bc1_data;
}

///////////////////////////////////////////////////////////////////////////////
// Image Loading
///////////////////////////////////////////////////////////////////////////////

static void _load_image(scene_bc1_t* scene, const char* path) {
	// Destroy existing textures if any
	if (skr_tex_is_valid(&scene->texture_original)) {
		skr_tex_destroy(&scene->texture_original);
		scene->texture_original = (skr_tex_t){0};
	}
	if (skr_tex_is_valid(&scene->texture_bc1)) {
		skr_tex_destroy(&scene->texture_bc1);
		scene->texture_bc1 = (skr_tex_t){0};
	}

	// Load source image
	int32_t      width, height;
	skr_tex_fmt_ format;
	void*        pixels = su_image_load(path, &width, &height, &format, 4);

	if (!pixels) {
		su_log(su_log_warning, "BC1: Failed to load image: %s", path);
		scene->img_width  = 0;
		scene->img_height = 0;
		return;
	}

	scene->img_width  = width;
	scene->img_height = height;

	// Create original texture
	skr_tex_create(skr_tex_fmt_rgba32_srgb,
		skr_tex_flags_readable,
		su_sampler_linear_clamp,
		(skr_vec3i_t){width, height, 1}, 1, 0,
		&(skr_tex_data_t){.data = pixels, .mip_count = 1, .layer_count = 1},
		&scene->texture_original);
	skr_tex_set_name(&scene->texture_original, "original");

	// Compress to BC1 (with timing)
	uint64_t start_ns = ska_time_get_elapsed_ns();
	uint8_t* bc1_data = _compress_to_bc1(pixels, width, height);
	uint64_t end_ns   = ska_time_get_elapsed_ns();
	double   time_ms  = (end_ns - start_ns) / 1000000.0;

	// BC1 data size is based on complete blocks, but image dimensions can be original
	int32_t  blocks_x = (width  + 3) / 4;
	int32_t  blocks_y = (height + 3) / 4;
	scene->bc1_size   = blocks_x * blocks_y * 8;

	su_log(su_log_info, "BC1: Compression took %.3f ms (%.1f MP/s)",
		time_ms, (width * height) / (time_ms * 1000.0));

	// Create BC1 texture with original dimensions - Vulkan handles block alignment internally
	skr_tex_create(skr_tex_fmt_bc1_rgb_srgb,
		skr_tex_flags_readable,
		su_sampler_linear_clamp,
		(skr_vec3i_t){width, height, 1}, 1, 0,
		&(skr_tex_data_t){.data = bc1_data, .mip_count = 1, .layer_count = 1},
		&scene->texture_bc1);
	skr_tex_set_name(&scene->texture_bc1, "bc1_compressed");

	// Update materials
	skr_material_set_tex(&scene->material_original, "tex", &scene->texture_original);
	skr_material_set_tex(&scene->material_bc1,      "tex", &scene->texture_bc1);

	free(bc1_data);
	su_image_free(pixels);

	su_log(su_log_info, "BC1: Compressed %dx%d image (%.1f KB -> %.1f KB, %.1f:1 ratio)",
		width, height,
		(width * height * 4) / 1024.0f,
		scene->bc1_size / 1024.0f,
		(float)(width * height * 4) / scene->bc1_size);
}

///////////////////////////////////////////////////////////////////////////////
// Scene Implementation
///////////////////////////////////////////////////////////////////////////////

static scene_t* _scene_bc1_create(void) {
	scene_bc1_t* scene = calloc(1, sizeof(scene_bc1_t));
	if (!scene) return NULL;

	scene->base.size    = sizeof(scene_bc1_t);
	scene->time         = 0.0f;
	scene->cam_distance = 5.0f;

	// Default file path
	strncpy(scene->file_path, "tree.png", sizeof(scene->file_path) - 1);

	// Create quad mesh for displaying textures (facing +Z)
	scene->quad_mesh = su_mesh_create_quad(2.0f, 2.0f, (skr_vec3_t){0, 0, 1}, false, (skr_vec4_t){1, 1, 1, 1});
	skr_mesh_set_name(&scene->quad_mesh, "bc1_quad");

	// Load unlit shader
	scene->shader = su_shader_load("shaders/unlit.hlsl.sks", "bc1_shader");

	// Create materials
	skr_material_create((skr_material_info_t){
		.shader     = &scene->shader,
		.cull       = skr_cull_back,
		.depth_test = skr_compare_less,
	}, &scene->material_original);

	skr_material_create((skr_material_info_t){
		.shader     = &scene->shader,
		.cull       = skr_cull_back,
		.depth_test = skr_compare_less,
	}, &scene->material_bc1);

	// Load default image
	_load_image(scene, scene->file_path);

	return (scene_t*)scene;
}

static void _scene_bc1_destroy(scene_t* base) {
	scene_bc1_t* scene = (scene_bc1_t*)base;

	skr_mesh_destroy    (&scene->quad_mesh);
	skr_material_destroy(&scene->material_original);
	skr_material_destroy(&scene->material_bc1);
	skr_shader_destroy  (&scene->shader);
	if (skr_tex_is_valid(&scene->texture_original)) skr_tex_destroy(&scene->texture_original);
	if (skr_tex_is_valid(&scene->texture_bc1))      skr_tex_destroy(&scene->texture_bc1);

	free(scene);
}

static void _scene_bc1_update(scene_t* base, float delta_time) {
	scene_bc1_t* scene = (scene_bc1_t*)base;
	scene->time += delta_time;

	// Handle load request from UI
	if (scene->load_requested) {
		scene->load_requested = false;
		_load_image(scene, scene->file_path);
	}

	// Camera input (only when not hovering UI)
	ImGuiIO* io = igGetIO();
	if (!io->WantCaptureMouse) {
		// Scroll wheel: zoom
		if (io->MouseWheel != 0.0f) {
			scene->cam_distance -= io->MouseWheel * 0.5f;
		}

		// Mouse drag: zoom (left button + vertical drag)
		if (io->MouseDown[0]) {
			scene->cam_distance += io->MouseDelta.y * 0.02f;
		}

		// Clamp distance
		if (scene->cam_distance < 1.0f)  scene->cam_distance = 1.0f;
		if (scene->cam_distance > 20.0f) scene->cam_distance = 20.0f;
	}
}

static void _scene_bc1_render(scene_t* base, int32_t width, int32_t height,
                              skr_render_list_t* ref_render_list,
                              su_system_buffer_t* ref_system_buffer) {
	scene_bc1_t* scene = (scene_bc1_t*)base;
	(void)width;
	(void)height;
	(void)ref_system_buffer;

	if (!skr_tex_is_valid(&scene->texture_original)) return;

	// Calculate aspect ratio for proper quad sizing
	float aspect = (float)scene->img_width / (float)scene->img_height;
	float quad_height = 2.0f;
	float quad_width  = quad_height * aspect;

	// Left quad: original texture at x=-1.5
	float4x4 left_world = float4x4_trs(
		(float3){-quad_width * 0.5f - 0.2f, 0.0f, 0.0f},
		(float4){0, 0, 0, 1},
		(float3){quad_width * 0.5f, quad_height * 0.5f, 1.0f});

	// Right quad: BC1 compressed at x=+1.5
	float4x4 right_world = float4x4_trs(
		(float3){quad_width * 0.5f + 0.2f, 0.0f, 0.0f},
		(float4){0, 0, 0, 1},
		(float3){quad_width * 0.5f, quad_height * 0.5f, 1.0f});

	skr_render_list_add(ref_render_list, &scene->quad_mesh, &scene->material_original, &left_world,  sizeof(float4x4), 1);
	skr_render_list_add(ref_render_list, &scene->quad_mesh, &scene->material_bc1,      &right_world, sizeof(float4x4), 1);
}

static bool _scene_bc1_get_camera(scene_t* base, scene_camera_t* out_camera) {
	scene_bc1_t* scene = (scene_bc1_t*)base;

	// Camera with zoom control
	out_camera->position = (float3){0, 0, scene->cam_distance};
	out_camera->target   = (float3){0, 0, 0};
	out_camera->up       = (float3){0, 1, 0};
	return true;
}

// Helper to get just the filename from a path
static const char* _get_filename(const char* path) {
	if (!path) return "(none)";
	const char* last_slash  = strrchr(path, '/');
	const char* last_bslash = strrchr(path, '\\');
	const char* name = path;
	if (last_slash  && last_slash  > name) name = last_slash  + 1;
	if (last_bslash && last_bslash > name) name = last_bslash + 1;
	return name;
}

static void _scene_bc1_render_ui(scene_t* base) {
	scene_bc1_t* scene = (scene_bc1_t*)base;

	igText("BC1 (DXT1) Compression Demo");
	igSeparator();

	// File loading
	igText("File: %s", _get_filename(scene->file_path));

	if (su_file_dialog_supported()) {
		if (igButton("Load Image...", (ImVec2){-1, 0})) {
			char* path = su_file_dialog_open("Select Image", "Image Files", "png;jpg;jpeg;bmp;tga");
			if (path) {
				strncpy(scene->file_path, path, sizeof(scene->file_path) - 1);
				scene->load_requested = true;
				free(path);
			}
		}
	} else {
		// Fallback: text input for platforms without file dialog
		igInputText("##path", scene->file_path, sizeof(scene->file_path), 0, NULL, NULL);
		igSameLine(0, 10);
		if (igButton("Load", (ImVec2){60, 0})) {
			scene->load_requested = true;
		}
	}

	igSeparator();

	// Image info
	if (scene->img_width > 0) {
		igText("Image: %d x %d", scene->img_width, scene->img_height);
		int32_t original_size = scene->img_width * scene->img_height * 4;
		igText("Original: %.1f KB (RGBA8)", original_size / 1024.0f);
		igText("BC1:      %.1f KB", scene->bc1_size / 1024.0f);
		igText("Ratio:    %.1f:1", (float)original_size / scene->bc1_size);
		igSeparator();
		igTextColored((ImVec4){0.7f, 0.7f, 0.7f, 1.0f}, "Left: Original  |  Right: BC1");
	} else {
		igTextColored((ImVec4){1.0f, 0.5f, 0.5f, 1.0f}, "No image loaded");
	}

	igSeparator();
	igText("Encoder: Simple min/max (fast)");
	igText("Quality: Acceptable for most uses");
}

const scene_vtable_t scene_bc1_vtable = {
	.name       = "BC1 Compression",
	.create     = _scene_bc1_create,
	.destroy    = _scene_bc1_destroy,
	.update     = _scene_bc1_update,
	.render     = _scene_bc1_render,
	.get_camera = _scene_bc1_get_camera,
	.render_ui  = _scene_bc1_render_ui,
};
