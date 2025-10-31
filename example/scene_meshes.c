// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "scene.h"
#include "scene_util.h"
#include "app.h"

#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#include <stdlib.h>
#include <string.h>

// Meshes scene - displays rotating cubes and pyramids with a stencil-masked sphere
typedef struct {
	scene_t       base;

	skr_mesh_t     cube_mesh;
	skr_mesh_t     pyramid_mesh;
	skr_mesh_t     sphere_mesh;
	skr_shader_t   shader;
	skr_material_t cube_material;
	skr_material_t pyramid_material;
	skr_material_t sphere_material;
	skr_tex_t      checkerboard_texture;
	skr_tex_t      white_texture;

	float rotation;
} scene_meshes_t;

static scene_t* _scene_meshes_create() {
	scene_meshes_t* scene = calloc(1, sizeof(scene_meshes_t));
	if (!scene) return NULL;

	scene->base.size = sizeof(scene_meshes_t);
	scene->rotation  = 0.0f;

	// Create cube mesh with per-face colors using utility function
	// Order: Front, Back, Top, Bottom, Right, Left
	skr_vec4_t cube_colors[6] = {
		{1.0f, 0.0f, 0.0f, 1.0f},  // Front: Red
		{0.0f, 1.0f, 0.0f, 1.0f},  // Back: Green
		{0.0f, 0.0f, 1.0f, 1.0f},  // Top: Blue
		{1.0f, 1.0f, 0.0f, 1.0f},  // Bottom: Yellow
		{1.0f, 0.0f, 1.0f, 1.0f},  // Right: Magenta
		{0.0f, 1.0f, 1.0f, 1.0f},  // Left: Cyan
	};
	scene->cube_mesh = su_mesh_create_cube(1.0f, cube_colors);
	skr_mesh_set_name(&scene->cube_mesh, "cube");

	// Create pyramid mesh using utility function
	skr_vec4_t white = {1.0f, 1.0f, 1.0f, 1.0f};
	scene->pyramid_mesh = su_mesh_create_pyramid(1.0f, 1.0f, white);
	skr_mesh_set_name(&scene->pyramid_mesh, "pyramid");

	// Create sphere mesh using utility function (16 segments, 12 rings)
	skr_vec4_t light_blue = {0.5f, 0.8f, 1.0f, 1.0f};
	scene->sphere_mesh = su_mesh_create_sphere(16, 12, 1.0f, light_blue);
	skr_mesh_set_name(&scene->sphere_mesh, "sphere");

	// Create textures using utility functions
	scene->checkerboard_texture = su_tex_create_checkerboard(512, 32, 0xFFFFFFFF, 0xFF000000, true);
	scene->white_texture        = su_tex_create_solid_color(0xFFFFFFFF);
	skr_tex_set_name(&scene->white_texture, "white_1x1");

	// Load shader
	scene->shader = su_shader_load("shaders/test.hlsl.sks", "main_shader");
	// Cube material: draws where stencil != 1 (outside sphere)
	skr_material_create((skr_material_info_t){
		.shader       = &scene->shader,
		.write_mask   = skr_write_default,
		.depth_test   = skr_compare_less,
		.stencil_front = {
			.compare      = skr_compare_equal,
			.compare_mask = 0xFF,
			.reference    = 1,
		},
	}, &scene->cube_material);
	skr_material_set_tex(&scene->cube_material, "tex", &scene->checkerboard_texture);

	// Pyramid material: draws where stencil != 1 (outside sphere)
	skr_material_create((skr_material_info_t){
		.shader       = &scene->shader,
		.write_mask   = skr_write_default,
		.depth_test   = skr_compare_less,
		.stencil_front = {
			.compare      = skr_compare_equal,
			.compare_mask = 0xFF,
			.reference    = 1,
		},
	}, &scene->pyramid_material);
	skr_material_set_tex(&scene->pyramid_material, "tex", &scene->white_texture);

	// Sphere material: draws first and marks stencil
	skr_material_create((skr_material_info_t){
		.shader       = &scene->shader,
		.write_mask   = skr_write_stencil,
		.depth_test   = skr_compare_less,
		.queue_offset = -100,  // Draw FIRST - before everything else
		.stencil_front = {
			.compare      = skr_compare_always,
			.pass_op      = skr_stencil_op_replace,
			.compare_mask = 0xFF,
			.write_mask   = 0xFF,
			.reference    = 1,  // Mark with value 1
		},
	}, &scene->sphere_material);
	skr_material_set_tex(&scene->sphere_material, "tex", &scene->white_texture);

	return (scene_t*)scene;
}

static void _scene_meshes_destroy(scene_t* base) {
	scene_meshes_t* scene = (scene_meshes_t*)base;

	skr_mesh_destroy(&scene->cube_mesh);
	skr_mesh_destroy(&scene->pyramid_mesh);
	skr_mesh_destroy(&scene->sphere_mesh);
	skr_material_destroy(&scene->cube_material);
	skr_material_destroy(&scene->pyramid_material);
	skr_material_destroy(&scene->sphere_material);
	skr_shader_destroy(&scene->shader);
	skr_tex_destroy(&scene->checkerboard_texture);
	skr_tex_destroy(&scene->white_texture);

	free(scene);
}

static void _scene_meshes_update(scene_t* base, float delta_time) {
	scene_meshes_t* scene = (scene_meshes_t*)base;
	scene->rotation += delta_time;
}

static void _scene_meshes_render(scene_t* base, int32_t width, int32_t height, HMM_Mat4 viewproj, skr_render_list_t* ref_render_list, app_system_buffer_t* ref_system_buffer) {
	scene_meshes_t* scene = (scene_meshes_t*)base;

	// Cubes (10x10 grid)
	for (int z = 0; z < 10; z++) {
		for (int x = 0; x < 10; x++) {
			HMM_Mat4 transform = su_matrix_trs(
				HMM_V3((x - 4.5f) * 1.5f, 0.0f, (z - 4.5f) * 1.5f),
				HMM_V3(0.0f, scene->rotation + (x + z) * 0.1f, 0.0f),
				HMM_V3(1.0f, 1.0f, 1.0f) );
			skr_render_list_add(ref_render_list, &scene->cube_mesh, &scene->cube_material, &transform, sizeof(HMM_Mat4), 1);
		}
	}

	// Pyramids (5 in a line)
	for (int i = 0; i < 5; i++) {
		HMM_Mat4 transform = su_matrix_trs(
			HMM_V3((i - 2.0f) * 3.0f, 2.0f, 0.0f),
			HMM_V3(0.0f, -scene->rotation * 2.0f, 0.0f),
			HMM_V3(1.0f, 1.0f, 1.0f) );
		skr_render_list_add(ref_render_list, &scene->pyramid_mesh, &scene->pyramid_material, &transform, sizeof(HMM_Mat4), 1);
	}

	// Sphere (center, slowly rotating, scale 3x)
	HMM_Mat4 sphere_transform = su_matrix_trs(
		HMM_V3(0.0f, 0.0f, 0.0f),
		HMM_V3(0.0f, scene->rotation * 0.5f, 0.0f),
		HMM_V3(5.0f, 5.0f, 5.0f) );
	skr_render_list_add(ref_render_list, &scene->sphere_mesh, &scene->sphere_material, &sphere_transform, sizeof(HMM_Mat4), 1);
}

const scene_vtable_t scene_meshes_vtable = {
	.name       = "Meshes (Cubes & Pyramids)",
	.create     = _scene_meshes_create,
	.destroy    = _scene_meshes_destroy,
	.update     = _scene_meshes_update,
	.render     = _scene_meshes_render,
	.get_camera = NULL,
};
