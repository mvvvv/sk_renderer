// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "scene.h"
#include "scene_util.h"
#include "app.h"

#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

// Simple CPU-based cloth simulation using Verlet integration
typedef struct {
	scene_t       base;

	skr_mesh_t     cloth_mesh;
	skr_shader_t   shader;
	skr_material_t material;
	skr_tex_t      texture;

	// Cloth simulation state
	HMM_Vec3*      positions;        // Current positions
	HMM_Vec3*      old_positions;    // Previous positions for Verlet integration
	bool*          pinned;           // Whether each vertex is pinned
	su_vertex_pnuc_t* vertices;     // Vertex buffer data
	uint32_t*      indices;          // Index buffer data

	int32_t        grid_width;
	int32_t        grid_height;
	int32_t        vertex_count;
	int32_t        index_count;
	float          time;

	// Tweakable simulation parameters
	float          gravity;
	float          damping;
	float          stiffness;
	int32_t        iterations;
} scene_cloth_t;

// Cloth parameters
#define CLOTH_WIDTH       16
#define CLOTH_HEIGHT      16
#define CLOTH_SIZE        5.0f
#define REST_DISTANCE     (CLOTH_SIZE / (CLOTH_WIDTH - 1))
#define GRAVITY           -0.2f   // Reduced gravity
#define DAMPING           0.99f  // More damping to reduce oscillation
#define STIFFNESS         0.6f    // Much stiffer springs
#define ITERATIONS        6       // More iterations for better constraint solving

static void _cloth_init(scene_cloth_t* scene) {
	scene->grid_width   = CLOTH_WIDTH;
	scene->grid_height  = CLOTH_HEIGHT;
	scene->vertex_count = CLOTH_WIDTH * CLOTH_HEIGHT;
	scene->index_count  = (CLOTH_WIDTH - 1) * (CLOTH_HEIGHT - 1) * 6;

	// Allocate arrays
	scene->positions     = calloc(scene->vertex_count, sizeof(HMM_Vec3));
	scene->old_positions = calloc(scene->vertex_count, sizeof(HMM_Vec3));
	scene->pinned        = calloc(scene->vertex_count, sizeof(bool));
	scene->vertices      = calloc(scene->vertex_count, sizeof(su_vertex_pnuc_t));
	scene->indices       = calloc(scene->index_count, sizeof(uint32_t));

	// Initialize cloth grid
	for (int32_t y = 0; y < CLOTH_HEIGHT; y++) {
		for (int32_t x = 0; x < CLOTH_WIDTH; x++) {
			int32_t idx = y * CLOTH_WIDTH + x;

			float fx = (float)x / (CLOTH_WIDTH - 1);
			float fy = (float)y / (CLOTH_HEIGHT - 1);

			HMM_Vec3 pos = HMM_V3(
				(fx - 0.5f) * CLOTH_SIZE,
				4.5f,
				(fy - 0.5f) * CLOTH_SIZE
			);

			scene->positions[idx] = pos;

			// Initialize old_positions with offset to give initial velocity matching gravity
			// Velocity = GRAVITY * TIME_STEP, so offset = -velocity * TIME_STEP
			HMM_Vec3 initial_velocity = HMM_V3(0, 0, 0);
			scene->old_positions[idx] = HMM_SubV3(pos, initial_velocity);

			// Pin top row
			scene->pinned[idx] = false;// (y == 0);

			// Set vertex data
			scene->vertices[idx].position = (skr_vec3_t){pos.X, pos.Y, pos.Z};
			scene->vertices[idx].normal   = (skr_vec3_t){0, 1, 0};
			scene->vertices[idx].uv       = (skr_vec2_t){fx, fy};
			scene->vertices[idx].color    = 0xFFFFFFFF;
		}
	}

	// Generate indices for triangles
	uint32_t idx = 0;
	for (int32_t y = 0; y < CLOTH_HEIGHT - 1; y++) {
		for (int32_t x = 0; x < CLOTH_WIDTH - 1; x++) {
			uint32_t tl = y * CLOTH_WIDTH + x;
			uint32_t tr = tl + 1;
			uint32_t bl = tl + CLOTH_WIDTH;
			uint32_t br = bl + 1;

			// First triangle
			scene->indices[idx++] = tl;
			scene->indices[idx++] = bl;
			scene->indices[idx++] = tr;

			// Second triangle
			scene->indices[idx++] = tr;
			scene->indices[idx++] = bl;
			scene->indices[idx++] = br;
		}
	}
}

static inline void _apply_distance_constraint(scene_cloth_t* scene, int32_t idx1, int32_t idx2, float rest_distance) {
	HMM_Vec3 p1    = scene->positions[idx1];
	HMM_Vec3 p2    = scene->positions[idx2];
	HMM_Vec3 delta = HMM_SubV3(p2, p1);
	float distance = HMM_LenV3(delta);

	if (distance > 0.0001f) {
		float    diff       = (distance - rest_distance) / distance;
		HMM_Vec3 correction = HMM_MulV3F(delta, diff * scene->stiffness * 0.5f);

		// Apply correction to both particles (unless pinned)
		if (!scene->pinned[idx1]) {
			scene->positions[idx1] = HMM_AddV3(p1, correction);
		}
		if (!scene->pinned[idx2]) {
			scene->positions[idx2] = HMM_SubV3(p2, correction);
		}
	}
}

static void _cloth_apply_constraints(scene_cloth_t* scene) {
	float rest_diagonal = REST_DISTANCE * 1.414f; // sqrt(2) for diagonal

	// Apply distance constraints between neighboring vertices
	for (int32_t iter = 0; iter < scene->iterations; iter++) {
		for (int32_t y = 0; y < CLOTH_HEIGHT; y++) {
			for (int32_t x = 0; x < CLOTH_WIDTH; x++) {
				int32_t idx = y * CLOTH_WIDTH + x;

				// Structural constraints - check all 4 directions
				// Right
				if (x < CLOTH_WIDTH - 1) {
					_apply_distance_constraint(scene, idx, idx + 1, REST_DISTANCE);
				}
				// Down
				if (y < CLOTH_HEIGHT - 1) {
					_apply_distance_constraint(scene, idx, idx + CLOTH_WIDTH, REST_DISTANCE);
				}

				// Diagonal constraints (shear) - only two to avoid double-processing
				// Down-right
				if (x < CLOTH_WIDTH - 1 && y < CLOTH_HEIGHT - 1) {
					_apply_distance_constraint(scene, idx, idx + CLOTH_WIDTH + 1, rest_diagonal);
				}
				// Down-left
				if (x > 0 && y < CLOTH_HEIGHT - 1) {
					_apply_distance_constraint(scene, idx, idx + CLOTH_WIDTH - 1, rest_diagonal);
				}
			}
		}
	}
}

static void _cloth_update_physics(scene_cloth_t* scene, float dt) {
	// Verlet integration
	for (int32_t i = 0; i < scene->vertex_count; i++) {
		if (scene->pinned[i]) continue;

		HMM_Vec3 pos     = scene->positions[i];
		HMM_Vec3 old_pos = scene->old_positions[i];

		// Velocity (implicit from position difference)
		HMM_Vec3 velocity = HMM_MulV3F(HMM_SubV3(pos, old_pos), scene->damping);

		// Add gravity
		velocity.Y += scene->gravity * dt;

		// Simple wind force (sine wave)
		float wind = sinf(scene->time * 2.0f + i * 0.1f) * 0.8f;
		velocity.Z += wind * dt;

		// Update position
		scene->old_positions[i] = pos;
		scene->positions[i]     = HMM_AddV3(pos, velocity);

		// Sphere collision (sphere at origin) with proper velocity preservation
		HMM_Vec3 sphere_center = HMM_V3(0, 0, 0);
		float    sphere_radius = 1.0f;
		HMM_Vec3 to_sphere     = HMM_SubV3(scene->positions[i], sphere_center);
		float    dist          = HMM_LenV3(to_sphere);

		if (dist < sphere_radius && dist > 0.0001f) {
			// Project particle to sphere surface
			HMM_Vec3 normal = HMM_MulV3F(to_sphere, 1.0f / dist);
			scene->positions[i] = HMM_AddV3(sphere_center, HMM_MulV3F(normal, sphere_radius));

			// Preserve tangential velocity by removing normal component from velocity
			// This prevents energy injection and maintains realistic sliding behavior
			HMM_Vec3 vel        = HMM_SubV3(scene->positions[i], scene->old_positions[i]);
			HMM_Vec3 normal_vel = HMM_MulV3F(normal, HMM_Dot(vel, normal));
			HMM_Vec3 tangent_vel = HMM_SubV3(vel, normal_vel);

			// Update old position to reflect new velocity (tangent only)
			scene->old_positions[i] = HMM_SubV3(scene->positions[i], tangent_vel);
		}
	}

	// Apply constraints
	_cloth_apply_constraints(scene);
}

static void _cloth_update_normals(scene_cloth_t* scene) {
	// Reset normals to zero
	HMM_Vec3* normals = calloc(scene->vertex_count, sizeof(HMM_Vec3));

	// Calculate face normals and accumulate
	for (int32_t i = 0; i < scene->index_count; i += 3) {
		uint32_t i0 = scene->indices[i + 0];
		uint32_t i1 = scene->indices[i + 1];
		uint32_t i2 = scene->indices[i + 2];

		HMM_Vec3 v0 = scene->positions[i0];
		HMM_Vec3 v1 = scene->positions[i1];
		HMM_Vec3 v2 = scene->positions[i2];

		HMM_Vec3 edge1  = HMM_SubV3(v1, v0);
		HMM_Vec3 edge2  = HMM_SubV3(v2, v0);
		HMM_Vec3 normal = HMM_Cross(edge1, edge2);

		normals[i0] = HMM_AddV3(normals[i0], normal);
		normals[i1] = HMM_AddV3(normals[i1], normal);
		normals[i2] = HMM_AddV3(normals[i2], normal);
	}

	// Normalize and convert to vertex data
	for (int32_t i = 0; i < scene->vertex_count; i++) {
		HMM_Vec3 n = HMM_NormV3(normals[i]);
		HMM_Vec3 p = scene->positions[i];

		scene->vertices[i].position = (skr_vec3_t){p.X, p.Y, p.Z};
		scene->vertices[i].normal   = (skr_vec3_t){n.X, n.Y, n.Z};
	}

	free(normals);
}

static scene_t* _scene_cloth_create(void) {
	scene_cloth_t* scene = calloc(1, sizeof(scene_cloth_t));
	if (!scene) return NULL;

	scene->base.size = sizeof(scene_cloth_t);
	scene->time      = 0.0f;

	// Initialize simulation parameters with defaults
	scene->gravity   = GRAVITY;
	scene->damping   = DAMPING;
	scene->stiffness = STIFFNESS;
	scene->iterations = ITERATIONS;

	// Initialize cloth simulation
	_cloth_init(scene);

	// Create mesh (start with static buffers, will convert to dynamic on first update)
	skr_mesh_create(
		&su_vertex_type_pnuc,
		skr_index_fmt_u32,
		scene->vertices,
		scene->vertex_count,
		scene->indices,
		scene->index_count,
		&scene->cloth_mesh
	);
	skr_mesh_set_name(&scene->cloth_mesh, "cloth");

	// Create texture
	scene->texture = su_tex_create_checkerboard(512, 32, 0xFFFFFFFF, 0xFF4444FF, true);
	skr_tex_set_name(&scene->texture, "cloth_texture");

	// Load shader
	scene->shader = su_shader_load("shaders/test.hlsl.sks", "cloth_shader");

	// Create material
	skr_material_create((skr_material_info_t){
		.shader     = &scene->shader,
		.write_mask = skr_write_default,
		.depth_test = skr_compare_less,
		.cull       = skr_cull_none,  // Two-sided
	}, &scene->material);
	skr_material_set_tex(&scene->material, "tex", &scene->texture);

	return (scene_t*)scene;
}

static void _scene_cloth_destroy(scene_t* base) {
	scene_cloth_t* scene = (scene_cloth_t*)base;

	skr_mesh_destroy(&scene->cloth_mesh);
	skr_material_destroy(&scene->material);
	skr_shader_destroy(&scene->shader);
	skr_tex_destroy(&scene->texture);

	free(scene->positions);
	free(scene->old_positions);
	free(scene->pinned);
	free(scene->vertices);
	free(scene->indices);
	free(scene);
}

static void _scene_cloth_update(scene_t* base, float delta_time) {
	scene_cloth_t* scene = (scene_cloth_t*)base;
	scene->time += delta_time;

	// Reset simulation every 5 seconds
	if (scene->time >= 2.2f) {
		scene->time = 0.0f;
		_cloth_init(scene);
	}

	// Fixed time step for stability
	_cloth_update_physics(scene, delta_time);
	_cloth_update_normals(scene);

	// Update mesh with new vertex data (converts to dynamic on second call)
	skr_mesh_set_verts(&scene->cloth_mesh, scene->vertices, scene->vertex_count);
}

static void _scene_cloth_render(scene_t* base, int32_t width, int32_t height, HMM_Mat4 viewproj, skr_render_list_t* ref_render_list, app_system_buffer_t* ref_system_buffer) {
	scene_cloth_t* scene = (scene_cloth_t*)base;

	// Draw cloth at origin
	HMM_Mat4 transform = su_matrix_trs(
		HMM_V3(0, 0, 0),
		HMM_V3(0, 0, 0),
		HMM_V3(1, 1, 1)
	);
	skr_render_list_add(ref_render_list, &scene->cloth_mesh, &scene->material, &transform, sizeof(HMM_Mat4), 1);
}

static void _scene_cloth_render_ui(scene_t* base) {
	scene_cloth_t* scene = (scene_cloth_t*)base;

	igText("Simulation Parameters:");
	igSliderFloat("Gravity", &scene->gravity, -1.0f, 0.0f, "%.2f", 0);
	igSliderFloat("Damping", &scene->damping, 0.9f, 1.0f, "%.3f", 0);
	igSliderFloat("Stiffness", &scene->stiffness, 0.1f, 1.0f, "%.2f", 0);
	igSliderInt("Iterations", &scene->iterations, 1, 12, "%d", 0);

	if (igButton("Reset Simulation", (ImVec2){-1, 0})) {
		scene->time = 0.0f;
		_cloth_init(scene);
	}
}

const scene_vtable_t scene_cloth_vtable = {
	.name        = "Cloth Sim (CPU)",
	.create      = _scene_cloth_create,
	.destroy     = _scene_cloth_destroy,
	.update      = _scene_cloth_update,
	.render      = _scene_cloth_render,
	.get_camera  = NULL,
	.render_ui   = _scene_cloth_render_ui,
};
