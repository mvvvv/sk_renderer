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
#include <math.h>

// Cubemap scene - displays a reflective sphere and skybox using a generated cubemap
typedef struct {
	scene_t       base;

	// Meshes
	skr_mesh_t     sphere_mesh;
	skr_mesh_t     skybox_mesh;

	// Shaders
	skr_shader_t   reflection_shader;
	skr_shader_t   skybox_shader;
	skr_shader_t   mipgen_shader;

	// Materials
	skr_material_t sphere_material;
	skr_material_t skybox_material;

	// Textures
	skr_tex_t      cubemap_texture;

	float rotation;
} scene_cubemap_t;

static scene_t* _scene_cubemap_create() {
	scene_cubemap_t* scene = calloc(1, sizeof(scene_cubemap_t));
	if (!scene) return NULL;

	scene->base.size = sizeof(scene_cubemap_t);
	scene->rotation  = 0.0f;

	// Create sphere mesh using utility function (32 segments, 24 rings for smooth reflections)
	skr_vec4_t white = {1.0f, 1.0f, 1.0f, 1.0f};
	scene->sphere_mesh = skr_mesh_create_sphere(32, 24, 1.0f, white);
	skr_mesh_set_name(&scene->sphere_mesh, "reflective_sphere");

	// Create fullscreen triangle for skybox
	scene->skybox_mesh = skr_mesh_create_fullscreen_quad();
	skr_mesh_set_name(&scene->skybox_mesh, "skybox_fullscreen_quad");

	// Create cubemap texture with different colors per face
	const int cube_size = 512;
	const int face_size = cube_size * cube_size;
	uint32_t* cubemap_data = malloc(face_size * 6 * sizeof(uint32_t));

	// Define colors for each cubemap face - distinct vibrant palette in linear color space
	// Order: +X (right), -X (left), +Y (top), -Y (bottom), +Z (front), -Z (back)
	// Converted from sRGB to linear RGB
	uint32_t face_colors[6] = {
		0xFF0F0AC9,  // +X: Bright Red (#E63946)
		0xFF0036ED,  // -X: Vibrant Orange (#F77F00)
		0xFF1084F8,  // +Y: Golden Yellow (#FCBF49)
		0xFF346200,  // +Z: Emerald Green (#06A77D)
		0xFF180903,  // -Y: Deep Blue (#1D3557)
		0xFF4A1224,  // -Z: Rich Purple (#6A4C93)
	};

	// Fill each face with its color
	for (int face = 0; face < 6; face++) {
		uint32_t* face_data = cubemap_data + (face * face_size);
		for (int i = 0; i < face_size; i++) {
			face_data[i] = face_colors[face];
		}
	}

	// Create cubemap texture (6-layer texture with cubemap flag)
	skr_tex_create(
		skr_tex_fmt_rgba32,
		skr_tex_flags_readable | skr_tex_flags_cubemap | skr_tex_flags_gen_mips,
		skr_sampler_linear_clamp,
		(skr_vec3i_t){cube_size, cube_size, 6},  // 6 faces
		1, 0, cubemap_data, &scene->cubemap_texture
	);
	skr_tex_set_name(&scene->cubemap_texture, "color_cubemap");
	free(cubemap_data);

	// Load cubemap mipgen shader for high-quality IBL filtering
	void*  shader_data = NULL;
	size_t shader_size = 0;
	if (app_read_file("shaders/cubemap_mipgen.hlsl.sks", &shader_data, &shader_size)) {
		skr_shader_create(shader_data, shader_size, &scene->mipgen_shader);
		skr_shader_set_name(&scene->mipgen_shader, "cubemap_mipgen");
		free(shader_data);
		shader_data = NULL;
	}

	// Generate mips for the cubemap using our custom shader
	skr_tex_generate_mips(&scene->cubemap_texture, &scene->mipgen_shader);

	// Load reflection shader
	if (app_read_file("shaders/cubemap_reflection.hlsl.sks", &shader_data, &shader_size)) {
		skr_shader_create(shader_data, shader_size, &scene->reflection_shader);
		skr_shader_set_name(&scene->reflection_shader, "reflection_shader");
		free(shader_data);

		if (skr_shader_is_valid(&scene->reflection_shader)) {
			skr_material_create((skr_material_info_t){
				.shader     = &scene->reflection_shader,
				.write_mask = skr_write_default,
				.depth_test = skr_compare_less,
			}, &scene->sphere_material);
			skr_material_set_tex(&scene->sphere_material, "cubemap", &scene->cubemap_texture);
		}
	}

	// Load skybox shader
	if (app_read_file("shaders/cubemap_skybox.hlsl.sks", &shader_data, &shader_size)) {
		skr_shader_create(shader_data, shader_size, &scene->skybox_shader);
		skr_shader_set_name(&scene->skybox_shader, "skybox_shader");
		free(shader_data);

		if (skr_shader_is_valid(&scene->skybox_shader)) {
			skr_material_create((skr_material_info_t){
				.shader       = &scene->skybox_shader,
				.write_mask   = skr_write_default,
				.depth_test   = skr_compare_less_or_eq,  // Less-equal for skybox
				.cull         = skr_cull_front,          // Cull front faces since we're inside
				.queue_offset = 100,                     // Draw last (after sphere)
			}, &scene->skybox_material);
			skr_material_set_tex(&scene->skybox_material, "cubemap", &scene->cubemap_texture);
		}
	}

	return (scene_t*)scene;
}

static void _scene_cubemap_destroy(scene_t* base) {
	scene_cubemap_t* scene = (scene_cubemap_t*)base;

	skr_mesh_destroy(&scene->sphere_mesh);
	skr_mesh_destroy(&scene->skybox_mesh);
	skr_material_destroy(&scene->sphere_material);
	skr_material_destroy(&scene->skybox_material);
	skr_shader_destroy(&scene->reflection_shader);
	skr_shader_destroy(&scene->skybox_shader);
	skr_shader_destroy(&scene->mipgen_shader);
	skr_tex_destroy(&scene->cubemap_texture);

	free(scene);
}

static void _scene_cubemap_update(scene_t* base, float delta_time) {
	scene_cubemap_t* scene = (scene_cubemap_t*)base;
	scene->rotation += delta_time;
}

static void _scene_cubemap_render(scene_t* base, int32_t width, int32_t height, HMM_Mat4 viewproj, skr_render_list_t* ref_render_list, app_system_buffer_t* ref_system_buffer) {
	scene_cubemap_t* scene = (scene_cubemap_t*)base;

	// Build instance data
	typedef struct {
		HMM_Mat4 world;
		float    roughness;
	} instance_data_t;

	// Create 3x3 grid of spheres with varying roughness
	const int grid_size = 3;
	instance_data_t sphere_instances[grid_size * grid_size];

	for (int z = 0; z < grid_size; z++) {
		for (int x = 0; x < grid_size; x++) {
			int idx = x + z * grid_size;

			// Position in grid (4 units apart)
			float xpos = (x - (grid_size - 1) * 0.5f) * 4.0f;
			float zpos = (z - (grid_size - 1) * 0.5f) * 4.0f;

			// Apply time-based animation to roughness
			float roughness_cycle = sinf(scene->rotation * 0.5f + x * 2 + z * 7) * 0.5f + 0.5f;
			float roughness = roughness_cycle;

			HMM_Mat4 sphere_transform = HMM_MulM4(
				HMM_Translate(HMM_V3(xpos, 0.0f, zpos)),
				HMM_MulM4(
					HMM_Scale(HMM_V3(1.5f, 1.5f, 1.5f)),
					HMM_Rotate_RH(scene->rotation * 0.3f + idx, HMM_V3(0.0f, 1.0f, 0.0f))
				)
			);

			sphere_instances[idx].world = HMM_Transpose(sphere_transform);
			sphere_instances[idx].roughness = roughness;
		}
	}

	// Add to the provided render list
	skr_render_list_add(ref_render_list, &scene->sphere_mesh, &scene->sphere_material, sphere_instances, sizeof(instance_data_t), grid_size * grid_size);
	skr_render_list_add(ref_render_list, &scene->skybox_mesh, &scene->skybox_material, NULL, 0, 1);
}

static bool _scene_cubemap_get_camera(scene_t* base, scene_camera_t* out_camera) {
	scene_cubemap_t* scene = (scene_cubemap_t*)base;

	// Orbit camera around the grid of spheres
	float radius = 12.0f;  // Further back to see whole grid
	float height = 4.0f;   // Higher up for better view
	float angle = scene->rotation * 0.4f;  // Smooth orbit

	out_camera->position = HMM_V3(cosf(angle) * radius, height, sinf(angle) * radius);
	out_camera->target   = HMM_V3(0.0f, 0.0f, 0.0f);  // Look at center of grid
	out_camera->up       = HMM_V3(0.0f, 1.0f, 0.0f);

	return true;  // Use this camera
}

const scene_vtable_t scene_cubemap_vtable = {
	.name       = "Cubemap (Reflection & Skybox)",
	.create     = _scene_cubemap_create,
	.destroy    = _scene_cubemap_destroy,
	.update     = _scene_cubemap_update,
	.render     = _scene_cubemap_render,
	.get_camera = _scene_cubemap_get_camera,
};
