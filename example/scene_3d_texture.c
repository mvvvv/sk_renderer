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

// 3D Texture scene - raymarches through a cube to visualize a 3D texture containing colored spheres

// Instance data: world matrix + inverse for local-space raymarching
typedef struct {
	float4x4 world;
	float4x4 world_inv;
} cube_instance_t;

typedef struct {
	scene_t        base;

	skr_mesh_t     cube_mesh;
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
		{ 0.3f, 0.55f, 0.5f, 0.3f,  0xFF0000FF },  // Red sphere (left)
		{ 0.5f, 0.3f,  0.5f, 0.2f,  0xFF00FF00 },  // Green sphere (center)
		{ 0.7f, 0.3f,  0.7f, 0.25f, 0xFFFF0000 },  // Blue sphere (right)
	};

	// Generate the 3D texture using SDF
	for (int32_t z = 0; z < size; z++) {
		for (int32_t y = 0; y < size; y++) {
			for (int32_t x = 0; x < size; x++) {
				// Normalize coordinates to 0-1
				float nx = (x + 0.5f) / size;
				float ny = (y + 0.5f) / size;
				float nz = (z + 0.5f) / size;

				// Calculate SDF distance for each sphere
				float    min_dist = 1;
				uint32_t color    = 0x00000000;
				for (int32_t i = 0; i < 3; i++) {
					sphere_t* s    = &spheres[i];
					float     dist = _sdf_sphere(nx, ny, nz, s->x, s->y, s->z, s->radius);
					if (min_dist > dist && dist < 0) {
						min_dist = dist;
						color    = s->color;
					}
				}

				int32_t idx = x + y * size + z * size * size;
				data[idx]   = color;
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

	// Create a cube mesh (size 1.0 = range -0.5 to 0.5 in local space)
	scene->cube_mesh = su_mesh_create_cube(1.0f, NULL);
	skr_mesh_set_name(&scene->cube_mesh, "raymarch_cube");

	// Load shader
	scene->shader = su_shader_load("shaders/texture3d.hlsl.sks", "texture3d_shader");
	skr_material_create((skr_material_info_t){
		.shader      = &scene->shader,
		.write_mask  = skr_write_default,
		.depth_test  = skr_compare_less,
		.cull        = skr_cull_back,
		.blend_state = skr_blend_alpha,
	}, &scene->material);

	// Create 3D texture with colored spheres
	const int32_t tex_size     = 64; // 64x64x64 volume
	uint32_t*     texture_data = _generate_3d_texture_data(tex_size);
	skr_tex_create(
		skr_tex_fmt_rgba32_srgb,
		skr_tex_flags_readable | skr_tex_flags_3d,
		(skr_tex_sampler_t){ .sample = skr_tex_sample_linear, .address = skr_tex_address_clamp },
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

	skr_mesh_destroy    (&scene->cube_mesh);
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

	// Slowly rotating cube, scaled up for better visibility
	cube_instance_t inst;
	inst.world = float4x4_trs(
		(float3){0.0f, 0.0f, 0.0f},
		float4_quat_from_euler((float3){scene->time * 0.3f, scene->time * 0.5f, 0.0f}),
		(float3){3.0f, 3.0f, 3.0f} );
	inst.world_inv = float4x4_invert(inst.world);

	skr_render_list_add(ref_render_list, &scene->cube_mesh, &scene->material, &inst, sizeof(cube_instance_t), 1);
}

const scene_vtable_t scene_3d_texture_vtable = {
	.name       = "3D Texture (Raymarch)",
	.create     = _scene_3d_texture_create,
	.destroy    = _scene_3d_texture_destroy,
	.update     = _scene_3d_texture_update,
	.render     = _scene_3d_texture_render,
	.get_camera = NULL,
};
