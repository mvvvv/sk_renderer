// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "scene.h"
#include "tools/scene_util.h"
#include "app.h"

#include <stdlib.h>
#include <string.h>

// Impostor scene - displays a textured impostor quad (two perpendicular quads)
typedef struct {
	scene_t       base;

	skr_mesh_t     impostor_mesh;
	skr_mesh_t     terrain_mesh;
	skr_shader_t   shader;
	skr_shader_t   mipgen_shader;
	skr_material_t tree_material;
	skr_material_t terrain_material;
	skr_tex_t      tree_texture;
	skr_tex_t      ground_texture;

	float rotation;
} scene_impostor_t;

// Helper function to calculate terrain height
static float _get_terrain_height(float x, float z) {
	return sinf(x * 0.2f) * cosf(z * 0.2f) * 2.0f;
}

static scene_t* _scene_impostor_create(void) {
	scene_impostor_t* scene = calloc(1, sizeof(scene_impostor_t));
	if (!scene) return NULL;

	scene->base.size = sizeof(scene_impostor_t);
	scene->rotation  = 0.0f;


	// Create impostor mesh - two perpendicular double-sided quads forming an X
	su_vertex_t impostor_vertices[] = {
		// First quad - front face (facing +Z)
		{ .position = {-0.5f, 0.0f, 0.0f}, .normal = {-1.0f, 0.0f, 0.0f}, .uv = {0.0f, 1.0f}, .color = 0xFFFFFFFF },
		{ .position = { 0.5f, 0.0f, 0.0f}, .normal = { 1.0f, 0.0f, 0.0f}, .uv = {1.0f, 1.0f}, .color = 0xFFFFFFFF },
		{ .position = { 0.5f, 1.0f, 0.0f}, .normal = { 1.0f, 1.0f, 0.0f}, .uv = {1.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = {-0.5f, 1.0f, 0.0f}, .normal = {-1.0f, 1.0f, 0.0f}, .uv = {0.0f, 0.0f}, .color = 0xFFFFFFFF },

		// Second quad - front face (facing +X)
		{ .position = {0.0f, 0.0f, -0.5f}, .normal = { 0.0f, 0.0f,-1.0f}, .uv = {0.0f, 1.0f}, .color = 0xFFFFFFFF },
		{ .position = {0.0f, 0.0f,  0.5f}, .normal = { 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = 0xFFFFFFFF },
		{ .position = {0.0f, 1.0f,  0.5f}, .normal = { 0.0f, 1.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = {0.0f, 1.0f, -0.5f}, .normal = { 0.0f, 1.0f,-1.0f}, .uv = {0.0f, 0.0f}, .color = 0xFFFFFFFF },
	};
	uint16_t impostor_indices[] = {
		0, 1, 2,  2, 3, 0,
		4, 6, 5,  6, 4, 7,
	};
	skr_mesh_create(&su_vertex_type, skr_index_fmt_u16, impostor_vertices, 8, impostor_indices, 12, &scene->impostor_mesh);
	skr_mesh_set_name(&scene->impostor_mesh, "impostor_quad");

	// Create terrain mesh - a grid with height field
	const int   grid_size    = 64;
	const float grid_spacing = 1.0f;
	const int   vertex_count = (grid_size + 1) * (grid_size + 1);
	const int   index_count  = grid_size * grid_size * 6;

	su_vertex_t* terrain_vertices = malloc(vertex_count * sizeof(su_vertex_t));
	uint16_t*         terrain_indices  = malloc(index_count  * sizeof(uint16_t));

	// Generate terrain vertices with height field
	for (int z = 0; z <= grid_size; z++) {
		for (int x = 0; x <= grid_size; x++) {
			int idx = x + z * (grid_size + 1);
			float world_x = (x - grid_size / 2.0f) * grid_spacing;
			float world_z = (z - grid_size / 2.0f) * grid_spacing;
			float height  = _get_terrain_height(world_x, world_z);

			terrain_vertices[idx].position = (skr_vec3_t){world_x, height, world_z};
			terrain_vertices[idx].normal   = (skr_vec3_t){0.0f, 1.0f, 0.0f};  // Will calculate proper normals below
			terrain_vertices[idx].uv       = (skr_vec2_t){(x / (float)grid_size) * 16.0f, (z / (float)grid_size) * 16.0f};
			terrain_vertices[idx].color    = 0xFFFFFFFF;  // Green grass color
		}
	}

	// Calculate proper normals
	for (int z = 0; z <= grid_size; z++) {
		for (int x = 0; x <= grid_size; x++) {
			int idx = x + z * (grid_size + 1);
			float3 normal = {0, 0, 0};

			// Sample neighboring heights to calculate normal
			float h_c = terrain_vertices[idx].position.y;
			float h_l = (x > 0) ? terrain_vertices[idx - 1].position.y : h_c;
			float h_r = (x < grid_size) ? terrain_vertices[idx + 1].position.y : h_c;
			float h_d = (z > 0) ? terrain_vertices[idx - (grid_size + 1)].position.y : h_c;
			float h_u = (z < grid_size) ? terrain_vertices[idx + (grid_size + 1)].position.y : h_c;

			float3 tangent_x = {grid_spacing, h_r - h_l, 0};
			float3 tangent_z = {0, h_u - h_d, grid_spacing};
			normal = float3_norm(float3_cross(tangent_z, tangent_x));

			terrain_vertices[idx].normal = (skr_vec3_t){normal.x, normal.y, normal.z};
		}
	}

	// Generate terrain indices
	int tri_idx = 0;
	for (int z = 0; z < grid_size; z++) {
		for (int x = 0; x < grid_size; x++) {
			int v0 = x + z * (grid_size + 1);
			int v1 = v0 + 1;
			int v2 = v0 + (grid_size + 1);
			int v3 = v2 + 1;

			terrain_indices[tri_idx++] = v0;
			terrain_indices[tri_idx++] = v2;
			terrain_indices[tri_idx++] = v1;

			terrain_indices[tri_idx++] = v1;
			terrain_indices[tri_idx++] = v2;
			terrain_indices[tri_idx++] = v3;
		}
	}

	skr_mesh_create(&su_vertex_type, skr_index_fmt_u16, terrain_vertices, vertex_count, terrain_indices, index_count, &scene->terrain_mesh);
	skr_mesh_set_name(&scene->terrain_mesh, "terrain");
	free(terrain_vertices);
	free(terrain_indices);

	// Load standard shader for both trees and terrain
	scene->shader = su_shader_load("shaders/test.hlsl.sks", "main_shader");

	// Load mipgen shader
	scene->mipgen_shader = su_shader_load("shaders/mipgen_alpha_weighted_render.hlsl.sks", "mipgen_shader");

	// Tree material with alpha-to-coverage for smooth edges
	skr_material_create((skr_material_info_t){
		.shader            = &scene->shader,
		.cull              = skr_cull_none,  // No culling so both sides are visible
		.write_mask        = skr_write_default,
		.depth_test        = skr_compare_less,
		.alpha_to_coverage = true,  // Alpha-to-coverage for smooth edges
	}, &scene->tree_material);

	// Terrain material
	skr_material_create((skr_material_info_t){
		.shader     = &scene->shader,
		.cull       = skr_cull_back,
		.write_mask = skr_write_default,
		.depth_test = skr_compare_less,
	}, &scene->terrain_material);

	// Load tree.png texture using image utility
	int32_t      width, height;
	skr_tex_fmt_ format;
	void*        pixels = su_image_load("tree.png", &width, &height, &format, 4);
	if (pixels) {
		skr_tex_create(format,
			skr_tex_flags_readable | skr_tex_flags_gen_mips,
			su_sampler_linear_clamp,
			(skr_vec3i_t){width, height, 1}, 1, 0,
			&(skr_tex_data_t){.data = pixels, .mip_count = 1, .layer_count = 1},
			&scene->tree_texture);
		skr_tex_set_name     (&scene->tree_texture, "tree");
		skr_tex_generate_mips(&scene->tree_texture, &scene->mipgen_shader);
		su_image_free(pixels);
	}
	skr_material_set_tex(&scene->tree_material, "tex", &scene->tree_texture);

	// Load ground.png texture for terrain
	pixels = su_image_load("ground.jpg", &width, &height, &format, 4);
	if (pixels) {
		skr_tex_create(format,
			skr_tex_flags_readable | skr_tex_flags_gen_mips,
			su_sampler_linear_wrap,  // Wrap for tiling
			(skr_vec3i_t){width, height, 1}, 1, 0,
			&(skr_tex_data_t){.data = pixels, .mip_count = 1, .layer_count = 1},
			&scene->ground_texture);
		skr_tex_set_name     (&scene->ground_texture, "ground");
		skr_tex_generate_mips(&scene->ground_texture, NULL);
		su_image_free(pixels);
	}
	skr_material_set_tex(&scene->terrain_material, "tex", &scene->ground_texture);

	return (scene_t*)scene;
}

static void _scene_impostor_destroy(scene_t* base) {
	scene_impostor_t* scene = (scene_impostor_t*)base;

	skr_mesh_destroy(&scene->impostor_mesh);
	skr_mesh_destroy(&scene->terrain_mesh);
	skr_material_destroy(&scene->tree_material);
	skr_material_destroy(&scene->terrain_material);
	skr_shader_destroy(&scene->shader);
	skr_shader_destroy(&scene->mipgen_shader);
	skr_tex_destroy(&scene->tree_texture);
	skr_tex_destroy(&scene->ground_texture);

	free(scene);
}

static void _scene_impostor_update(scene_t* base, float delta_time) {
	scene_impostor_t* scene = (scene_impostor_t*)base;
	scene->rotation += delta_time * 0.5f;
}

static void _scene_impostor_render(scene_t* base, int32_t width, int32_t height, skr_render_list_t* ref_render_list, su_system_buffer_t* ref_system_buffer) {
	scene_impostor_t* scene = (scene_impostor_t*)base;

	// Build instance data - 1000 randomly placed trees
	float4x4* instances = malloc(1000 * sizeof(float4x4));

	// Simple hash function for consistent random placement
	for (int i = 0; i < 1000; i++) {
		uint32_t hash = i * 2654435761u;
		float x = ((hash & 0xFFFF) / 65535.0f - 0.5f) * 50.0f;  // -25 to +25
		hash = hash * 2654435761u;
		float z = ((hash & 0xFFFF) / 65535.0f - 0.5f) * 50.0f;  // -25 to +25
		hash = hash * 2654435761u;
		float rot = (hash / (float)0xFFFFFFFF) * 3.14159f * 2.0f;
		hash = hash * 2654435761u;
		float scale = 1.0f + ((hash & 0xFFFF) / 65535.0f) * 1.5f;  // 0.7 to 1.3

		// Get terrain height for tree placement
		float y = _get_terrain_height(x, z);

		instances[i] = float4x4_trs(
			(float3){x, y, z},
			float4_quat_from_euler((float3){0.0f, rot, 0.0f}),
			(float3){scale, scale * 2.0f, scale} );  // 2x taller than wide, with random scale
	}

	// Add to render list
	// First: Render terrain
	float4x4 terrain_instance = float4x4_trs(
		(float3){0.0f, 0.0f, 0.0f},
		(float4){0, 0, 0, 1},
		(float3){1.0f, 1.0f, 1.0f} );
	skr_render_list_add(ref_render_list, &scene->terrain_mesh, &scene->terrain_material, &terrain_instance, sizeof(float4x4), 1);

	// Second: Render trees with alpha-to-coverage for smooth edges
	skr_render_list_add(ref_render_list, &scene->impostor_mesh, &scene->tree_material, instances, sizeof(float4x4), 1000);

	free(instances);
}

static bool _scene_impostor_get_camera(scene_t* base, scene_camera_t* out_camera) {
	scene_impostor_t* scene = (scene_impostor_t*)base;

	// Orbit camera around the forest
	float radius = 20.0f;
	float height = 7.0f;
	float angle = scene->rotation * 0.3f;  // Slow orbit

	out_camera->position = (float3){cosf(angle) * radius, height, sinf(angle) * radius};
	out_camera->target   = (float3){0.0f, 1.0f, 0.0f};  // Look at center, slightly up
	out_camera->up       = (float3){0.0f, 1.0f, 0.0f};

	return true;  // Use this camera
}

const scene_vtable_t scene_impostor_vtable = {
	.name       = "Impostor Quad (Tree)",
	.create     = _scene_impostor_create,
	.destroy    = _scene_impostor_destroy,
	.update     = _scene_impostor_update,
	.render     = _scene_impostor_render,
	.get_camera = _scene_impostor_get_camera,
};
