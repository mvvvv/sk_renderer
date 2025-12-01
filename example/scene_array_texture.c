// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "scene.h"
#include "scene_util.h"
#include "app.h"

#include <stdlib.h>
#include <string.h>

// Array texture scene - renders cubes to a 2-layer array texture, displays as red/cyan stereo
typedef struct {
	scene_t            base;
	skr_render_list_t  render_list;

	// 3D rendering (cubes to array texture)
	skr_mesh_t     cube_mesh;
	skr_shader_t   cube_shader;
	skr_material_t cube_material;
	skr_tex_t      checkerboard_texture;
	skr_tex_t      array_render_target;  // 2-layer array texture
	skr_tex_t      depth_buffer;

	// Stereo display (array texture to screen)
	skr_mesh_t     fullscreen_quad;
	skr_shader_t   stereo_shader;
	skr_material_t stereo_material;

	float rotation;
	float eye_separation;
} scene_array_texture_t;

static scene_t* _scene_array_texture_create(void) {
	scene_array_texture_t* scene = calloc(1, sizeof(scene_array_texture_t));
	if (!scene) return NULL;

	scene->base.size      = sizeof(scene_array_texture_t);
	scene->rotation       = 0.0f;
	scene->eye_separation = 0.2f;
	skr_render_list_create(&scene->render_list);

	// Create cube mesh with per-face colors using utility function
	// Order: Front, Back, Top, Bottom, Right, Left
	skr_vec4_t face_colors[6] = {
		{1.0f, 0.5f, 0.5f, 1.0f},  // Front: Red
		{0.5f, 1.0f, 0.5f, 1.0f},  // Back: Green
		{0.5f, 0.5f, 1.0f, 1.0f},  // Top: Blue
		{1.0f, 1.0f, 0.5f, 1.0f},  // Bottom: Yellow
		{1.0f, 0.5f, 1.0f, 1.0f},  // Right: Magenta
		{0.5f, 1.0f, 1.0f, 1.0f},  // Left: Cyan
	};
	scene->cube_mesh = su_mesh_create_cube(1.0f, face_colors);
	skr_mesh_set_name(&scene->cube_mesh, "stereo_cube");

	// Create fullscreen quad for stereo display
	scene->fullscreen_quad = su_mesh_create_fullscreen_quad();
	skr_mesh_set_name(&scene->fullscreen_quad, "stereo_quad");

	// Load cube shader
	scene->cube_shader = su_shader_load("shaders/test.hlsl.sks", "cube_shader");
	skr_material_create((skr_material_info_t){
		.shader     = &scene->cube_shader,
		.write_mask = skr_write_default,
		.depth_test = skr_compare_less,
	}, &scene->cube_material);

	// Load stereo display shader
	scene->stereo_shader = su_shader_load("shaders/stereo_display.hlsl.sks", "stereo_shader");
	skr_material_create((skr_material_info_t){
		.shader     = &scene->stereo_shader,
		.cull       = skr_cull_none,
		.write_mask = skr_write_rgba,
		.depth_test = skr_compare_always,
	}, &scene->stereo_material);

	// Create checkerboard texture using utility function
	scene->checkerboard_texture = su_tex_create_checkerboard(512, 32, 0xFFFFFFFF, 0xFF000000, true);
	skr_tex_set_name(&scene->checkerboard_texture, "checkerboard");

	// Create 2-layer array texture (rendered target) - will be created in resize
	scene->array_render_target.image = VK_NULL_HANDLE;
	scene->depth_buffer.image        = VK_NULL_HANDLE;

	// Bind textures to materials
	skr_material_set_tex(&scene->cube_material, "tex", &scene->checkerboard_texture);

	return (scene_t*)scene;
}

static void _scene_array_texture_destroy(scene_t* base) {
	scene_array_texture_t* scene = (scene_array_texture_t*)base;

	skr_render_list_destroy(&scene->render_list);
	skr_mesh_destroy(&scene->cube_mesh);
	skr_mesh_destroy(&scene->fullscreen_quad);
	skr_material_destroy(&scene->cube_material);
	skr_material_destroy(&scene->stereo_material);
	skr_shader_destroy(&scene->cube_shader);
	skr_shader_destroy(&scene->stereo_shader);
	skr_tex_destroy(&scene->checkerboard_texture);
	skr_tex_destroy(&scene->array_render_target);
	skr_tex_destroy(&scene->depth_buffer);

	free(scene);
}

static void _scene_array_texture_update(scene_t* base, float delta_time) {
	scene_array_texture_t* scene = (scene_array_texture_t*)base;
	scene->rotation += delta_time;
}

static void _scene_array_texture_render(scene_t* base, int32_t width, int32_t height, float4x4 viewproj, skr_render_list_t* ref_render_list, app_system_buffer_t* ref_system_buffer) {
	scene_array_texture_t* scene = (scene_array_texture_t*)base;

	// Create/resize array texture if needed
	if (!skr_tex_is_valid(&scene->array_render_target) ||
	    scene->array_render_target.size.x != width ||
	    scene->array_render_target.size.y != height) {

		if (skr_tex_is_valid(&scene->array_render_target)) {
			skr_tex_destroy(&scene->array_render_target);
			skr_tex_destroy(&scene->depth_buffer);
		}

		// Create 2-layer array texture
		skr_tex_create(
			skr_tex_fmt_rgba32_srgb,
			skr_tex_flags_writeable | skr_tex_flags_readable | skr_tex_flags_array,
			su_sampler_linear_clamp,
			(skr_vec3i_t){width, height, 2},  // 2 layers
			1, 0, NULL, &scene->array_render_target
		);
		skr_tex_set_name(&scene->array_render_target, "array_stereo_rt");

		// Create depth buffer
		skr_tex_create(
			skr_tex_fmt_depth32,
			skr_tex_flags_writeable | skr_tex_flags_array,
			su_sampler_linear_clamp,
			(skr_vec3i_t){width, height, 2},  // 2 layers
			1, 0, NULL, &scene->depth_buffer
		);
		skr_tex_set_name(&scene->depth_buffer, "array_stereo_depth");

		// Bind array texture to stereo material
		skr_material_set_tex(&scene->stereo_material, "array_tex", &scene->array_render_target);
	}

	// Build stereo system buffer (2 views for left/right eye)
	app_system_buffer_t sys_buffer = {0};
	sys_buffer.view_count = 2;

	// Extract projection matrix from viewproj passed from app.c
	float4x4 projection;
	memcpy(&projection, ref_system_buffer->projection[0], sizeof(float) * 16);

	// Create view matrices for left and right eye
	float3 camera_pos = {0, 3, 8};  // Match app.c default camera
	float3 target     = {0, 0, 0};
	float3 up         = {0, 1, 0};

	// Left eye (offset to the left)
	float3   eye_sep_left = {-scene->eye_separation * 0.5f, 0, 0};
	float4x4 view_left    = float4x4_lookat(float3_add(camera_pos, eye_sep_left), float3_add(target, eye_sep_left), up);

	// Right eye (offset to the right)
	float3   eye_sep_right = {scene->eye_separation * 0.5f, 0, 0};
	float4x4 view_right    = float4x4_lookat(float3_add(camera_pos, eye_sep_right), float3_add(target, eye_sep_right), up);

	// Calculate inverse matrices
	float4x4 view_left_inv   = float4x4_invert(view_left);
	float4x4 view_right_inv  = float4x4_invert(view_right);
	float4x4 projection_inv  = float4x4_invert(projection);

	// Copy to system buffer
	memcpy(sys_buffer.view          [0], &view_left,       sizeof(float) * 16);
	memcpy(sys_buffer.view          [1], &view_right,      sizeof(float) * 16);
	memcpy(sys_buffer.view_inv      [0], &view_left_inv,   sizeof(float) * 16);
	memcpy(sys_buffer.view_inv      [1], &view_right_inv,  sizeof(float) * 16);
	memcpy(sys_buffer.projection    [0], &projection,      sizeof(float) * 16);
	memcpy(sys_buffer.projection    [1], &projection,      sizeof(float) * 16);
	memcpy(sys_buffer.projection_inv[0], &projection_inv,  sizeof(float) * 16);
	memcpy(sys_buffer.projection_inv[1], &projection_inv,  sizeof(float) * 16);

	// Compute viewproj matrices
	float4x4 viewproj_left  = float4x4_mul(projection, view_left);
	float4x4 viewproj_right = float4x4_mul(projection, view_right);
	memcpy(sys_buffer.viewproj[0], &viewproj_left,  sizeof(float) * 16);
	memcpy(sys_buffer.viewproj[1], &viewproj_right, sizeof(float) * 16);

	// Calculate camera positions and directions for both eyes
	float3 cam_pos_left  = float3_add(camera_pos, (float3){-scene->eye_separation * 0.5f, 0, 0});
	float3 cam_pos_right = float3_add(camera_pos, (float3){ scene->eye_separation * 0.5f, 0, 0});
	float3 cam_forward   = float3_norm(float3_sub(target, camera_pos));

	sys_buffer.cam_pos[0][0] = cam_pos_left.x;
	sys_buffer.cam_pos[0][1] = cam_pos_left.y;
	sys_buffer.cam_pos[0][2] = cam_pos_left.z;
	sys_buffer.cam_pos[0][3] = 0.0f;
	sys_buffer.cam_pos[1][0] = cam_pos_right.x;
	sys_buffer.cam_pos[1][1] = cam_pos_right.y;
	sys_buffer.cam_pos[1][2] = cam_pos_right.z;
	sys_buffer.cam_pos[1][3] = 0.0f;
	sys_buffer.cam_dir[0][0] = cam_forward.x;
	sys_buffer.cam_dir[0][1] = cam_forward.y;
	sys_buffer.cam_dir[0][2] = cam_forward.z;
	sys_buffer.cam_dir[0][3] = 0.0f;
	sys_buffer.cam_dir[1][0] = cam_forward.x;
	sys_buffer.cam_dir[1][1] = cam_forward.y;
	sys_buffer.cam_dir[1][2] = cam_forward.z;
	sys_buffer.cam_dir[1][3] = 0.0f;

	// Build cube instance data (configurable grid)
	#define       grid_size_x 100
	#define       grid_size_z 100
	const float   spacing     = 2.0f;
	const int32_t total_cubes = grid_size_x * grid_size_z;

	float4x4 cube_instances[grid_size_x*grid_size_z];

	for (int z = 0; z < grid_size_z; z++) {
		for (int x = 0; x < grid_size_x; x++) {
			int   idx  = x + z * grid_size_x;
			float xpos = (x - grid_size_x * 0.5f + 0.5f) * spacing;
			float zpos = (z - grid_size_z * 0.5f + 0.5f) * spacing;
			float yrot = scene->rotation + (x + z) * 0.2f;
			cube_instances[idx] = float4x4_trs(
				(float3){xpos, 0.0f, zpos},
				float4_quat_from_euler((float3){0.0f, yrot, 0.0f}),
				(float3){1.0f, 1.0f, 1.0f}
			);
		}
	}

	// Render cubes to array texture (separate render pass)
	skr_renderer_begin_pass(&scene->array_render_target, &scene->depth_buffer, NULL, skr_clear_all, (skr_vec4_t){0.0f, 0.0f, 0.0f, 0.0f}, 1.0f, 0);
	skr_renderer_set_viewport((skr_rect_t ){0, 0, (float)scene->array_render_target.size.x, (float)scene->array_render_target.size.y});
	skr_renderer_set_scissor ((skr_recti_t){0, 0, scene->array_render_target.size.x, scene->array_render_target.size.y});

	skr_render_list_add  (&scene->render_list, &scene->cube_mesh, &scene->cube_material, cube_instances, sizeof(float4x4), total_cubes);
	skr_renderer_draw    (&scene->render_list, &sys_buffer, sizeof(app_system_buffer_t), sys_buffer.view_count);
	skr_render_list_clear(&scene->render_list);
	skr_renderer_end_pass();

	// Display array texture as red/cyan stereo to swapchain (in the main render pass)
	skr_render_list_add(ref_render_list, &scene->fullscreen_quad, &scene->stereo_material, NULL, 0, 1);
}

const scene_vtable_t scene_array_texture_vtable = {
	.name       = "Array Texture Stereo",
	.create     = _scene_array_texture_create,
	.destroy    = _scene_array_texture_destroy,
	.update     = _scene_array_texture_update,
	.render     = _scene_array_texture_render,
	.get_camera = NULL,
};
