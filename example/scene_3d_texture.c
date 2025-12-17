// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "scene.h"
#include "tools/scene_util.h"
#include "app.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

// 3D Texture scene - displays a quad that moves up and down, sampling from a 3D texture containing colored spheres
typedef struct {
	scene_t        base;

	skr_mesh_t     quad_mesh;
	skr_shader_t   shader;
	skr_material_t material;
	skr_tex_t      texture_3d;

	float          time;
} scene_3d_texture_t;

// SDF helper: sphere distance
static inline float _sdf_sphere(float x, float y, float z, float cx, float cy, float cz, float radius) {
	float dx = x - cx;
	float dy = y - cy;
	float dz = z - cz;
	return sqrtf(dx*dx + dy*dy + dz*dz) - radius;
}

// Helper function to generate 3D texture data with 3 colored spheres using SDF
static uint32_t* _generate_3d_texture_data(int32_t size) {
	uint32_t* data = malloc(size * size * size * sizeof(uint32_t));
	if (!data) return NULL;

	// Define 3 spheres at different positions with different colors
	typedef struct {
		float    x, y, z;     // Position (normalized 0-1)
		float    radius;      // Radius (normalized)
		uint32_t color;       // RGBA color
	} sphere_t;

	sphere_t spheres[] = {
		{ 0.3f, 0.55f, 0.5f, 0.3f, 0xFF0000FF },  // Red sphere (left)
		{ 0.5f, 0.3f,  0.5f, 0.2f, 0xFF00FF00 },  // Green sphere (center)
		{ 0.7f, 0.3f,  0.7f, 0.25f, 0xFFFF0000 },  // Blue sphere (right)
	};

	const float smooth_k = 0.1f;  // Smoothness factor for blending

	// Generate the 3D texture using SDF
	for (int32_t z = 0; z < size; z++) {
		for (int32_t y = 0; y < size; y++) {
			for (int32_t x = 0; x < size; x++) {
				// Normalize coordinates to 0-1
				float nx = (x + 0.5f) / size;
				float ny = (y + 0.5f) / size;
				float nz = (z + 0.5f) / size;

				// Calculate SDF distance for each sphere
				float    min   = 1;
				uint32_t color = 0x00000000;
				for (int32_t i = 0; i < 3; i++) {
					sphere_t* s = &spheres[i];
					float dist = _sdf_sphere(nx, ny, nz, s->x, s->y, s->z, s->radius);
					if (min > dist && dist < 0) {
						min   = dist;
						color = s->color;
					}
				}

				int32_t idx = x + y * size + z * size * size;
				data[idx] = color;
			}
		}
	}

	return data;
}

static scene_t* _scene_3d_texture_create(void) {
	scene_3d_texture_t* scene = calloc(1, sizeof(scene_3d_texture_t));
	if (!scene) return NULL;

	scene->base.size = sizeof(scene_3d_texture_t);
	scene->time      = 0.0f;

	// Create a flat quad mesh (horizontal, on XZ plane)
	su_vertex_t quad_vertices[] = {
		{ .position = {-2.0f, 0.0f, -2.0f}, .normal = {0.0f, 1.0f, 0.0f}, .uv = {0.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = { 2.0f, 0.0f, -2.0f}, .normal = {0.0f, 1.0f, 0.0f}, .uv = {1.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = { 2.0f, 0.0f,  2.0f}, .normal = {0.0f, 1.0f, 0.0f}, .uv = {1.0f, 1.0f}, .color = 0xFFFFFFFF },
		{ .position = {-2.0f, 0.0f,  2.0f}, .normal = {0.0f, 1.0f, 0.0f}, .uv = {0.0f, 1.0f}, .color = 0xFFFFFFFF },
	};
	uint16_t quad_indices[] = {
		0, 1, 2,
		2, 3, 0,
	};
	skr_mesh_create  (&su_vertex_type, skr_index_fmt_u16, quad_vertices, 4, quad_indices, 6, &scene->quad_mesh);
	skr_mesh_set_name(&scene->quad_mesh, "quad");

	// Load shader
	scene->shader = su_shader_load("shaders/texture3d.hlsl.sks", "texture3d_shader");
	skr_material_create((skr_material_info_t){
		.shader     = &scene->shader,
		.write_mask = skr_write_default,
		.depth_test = skr_compare_less,
		.cull       = skr_cull_none,
		.alpha_to_coverage = true,
	}, &scene->material);

	// Create 3D texture with colored spheres
	const int32_t tex_size     = 64; // 64x64x64 volume
	uint32_t*     texture_data = _generate_3d_texture_data(tex_size);
	skr_tex_create(
		skr_tex_fmt_rgba32_srgb,
		skr_tex_flags_readable | skr_tex_flags_3d,
		(skr_tex_sampler_t){ .sample  = skr_tex_sample_linear, .address = skr_tex_address_clamp },
		(skr_vec3i_t){tex_size, tex_size, tex_size},
		1,
		1,
		&(skr_tex_data_t){.data = texture_data, .mip_count = 1, .layer_count = 1},
		&scene->texture_3d );
	free(texture_data);
	skr_tex_set_name    (&scene->texture_3d, "3d_spheres");
	skr_material_set_tex(&scene->material,   "tex", &scene->texture_3d);

	return (scene_t*)scene;
}

static void _scene_3d_texture_destroy(scene_t* base) {
	scene_3d_texture_t* scene = (scene_3d_texture_t*)base;

	skr_mesh_destroy    (&scene->quad_mesh);
	skr_material_destroy(&scene->material);
	skr_shader_destroy  (&scene->shader);
	skr_tex_destroy     (&scene->texture_3d);

	free(scene);
}

static void _scene_3d_texture_update(scene_t* base, float delta_time) {
	scene_3d_texture_t* scene = (scene_3d_texture_t*)base;
	scene->time += delta_time;
}

static void _scene_3d_texture_render(scene_t* base, int32_t width, int32_t height, skr_render_list_t* ref_render_list, su_system_buffer_t* ref_system_buffer) {
	scene_3d_texture_t* scene = (scene_3d_texture_t*)base;

	float4x4 quad_instances[2];

	// First quad: moves up and down (horizontal)
	quad_instances[0] = float4x4_trs(
		(float3){0.0f, sinf(scene->time * 2.0f) * 2.0f, 0.0f},
		(float4){0, 0, 0, 1},
		(float3){1.0f, 1.0f, 1.0f} );
	// Second quad: spins around Y axis (vertical, standing up)
	quad_instances[1] = float4x4_trs(
		(float3){0.0f, 0.0f, 0.0f},
		float4_quat_from_euler((float3){1.5708f, scene->time * 1.5f, 0.0f}),
		(float3){1.0f, 1.0f, 1.0f} );

	// Add both quads to the provided render list
	skr_render_list_add(ref_render_list, &scene->quad_mesh, &scene->material, quad_instances, sizeof(float4x4), 2);
}

const scene_vtable_t scene_3d_texture_vtable = {
	.name       = "3D Texture (Sphere Slices)",
	.create     = _scene_3d_texture_create,
	.destroy    = _scene_3d_texture_destroy,
	.update     = _scene_3d_texture_update,
	.render     = _scene_3d_texture_render,
	.get_camera = NULL,
};
