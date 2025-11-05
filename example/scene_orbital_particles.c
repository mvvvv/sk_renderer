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
#include <float.h>

#define PARTICLE_COUNT 250000
#define ATTRACTOR_COUNT 3


// Create particle params buffer for rendering (colors)
typedef struct {
	HMM_Vec3 color_slow;
	float    max_speed;
	HMM_Vec3 color_fast;
	float    _pad;
} particle_params_t;

// Orbital particles scene - displays particles orbiting around moving attractors
typedef struct {
	scene_t           base;
	skr_mesh_t        pyramid_mesh;
	skr_shader_t      shader;
	skr_shader_t      compute_shader;
	skr_material_t    material;
	skr_tex_t         white_texture;
	particle_params_t particle_params;
	skr_compute_t     compute_ping;
	skr_compute_t     compute_pong;
	skr_buffer_t      particle_buffer_a;
	skr_buffer_t      particle_buffer_b;
	skr_buffer_t      compute_params_buffer;

	float   time;
	int32_t compute_iteration;
} scene_orbital_particles_t;

// Particle data
typedef struct {
	HMM_Vec3 position;
	HMM_Vec3 velocity;
} particle_t;

// Instance data - just translation!
typedef struct {
	HMM_Vec3 position;
} instance_data_t;

// Helper function for random hash
static float _hash_f(int32_t aPosition, uint32_t aSeed) {
	const uint32_t BIT_NOISE1 = 0x68E31DA4;
	const uint32_t BIT_NOISE2 = 0xB5297A4D;
	const uint32_t BIT_NOISE3 = 0x1B56C4E9;

	uint32_t mangled = (uint32_t)aPosition;
	mangled *= BIT_NOISE1;
	mangled += aSeed;
	mangled ^= (mangled >> 8);
	mangled += BIT_NOISE2;
	mangled ^= (mangled << 8);
	mangled *= BIT_NOISE3;
	mangled ^= (mangled >> 8);
	return (float)mangled / (float)4294967295;
}

static scene_t* _scene_orbital_particles_create(void) {
	scene_orbital_particles_t* scene = calloc(1, sizeof(scene_orbital_particles_t));
	if (!scene) return NULL;

	scene->base.size         = sizeof(scene_orbital_particles_t);
	scene->time              = 0.0f;
	scene->compute_iteration = 0;

	skr_vec4_t color = {1,1,1,1};

	// Create simple 3-sided pyramid (tetrahedron) mesh
	const float h = 0.5f;  // Height
	const float r = 0.5f;  // Base radius
	su_vertex_pnuc_t pyramid_vertices[] = {
		// Base triangle
		{ .position = { 0.0f,    -h/2, 0.0f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {0.5f, 0.5f}, .color = 0xFFFFFFFF },
		{ .position = { r,       -h/2, 0.0f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {1.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = {-r*0.5f,  -h/2,  r*0.866f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {0.0f, 1.0f}, .color = 0xFFFFFFFF },
		{ .position = {-r*0.5f,  -h/2, -r*0.866f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {0.0f, 0.0f}, .color = 0xFFFFFFFF },
		// Apex
		{ .position = { 0.0f,     h/2, 0.0f}, .normal = { 0.0f,  1.0f,  0.0f}, .uv = {0.5f, 0.5f}, .color = 0xFFFFFFFF },
		// Front right face
		{ .position = { 0.0f,    -h/2, 0.0f}, .normal = { 0.866f, 0.5f,  0.0f}, .uv = {0.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = { r,       -h/2, 0.0f}, .normal = { 0.866f, 0.5f,  0.0f}, .uv = {1.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = { 0.0f,     h/2, 0.0f}, .normal = { 0.866f, 0.5f,  0.0f}, .uv = {0.5f, 1.0f}, .color = 0xFFFFFFFF },
		// Back left face
		{ .position = { r,       -h/2, 0.0f}, .normal = {-0.433f, 0.5f,  0.75f}, .uv = {0.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = {-r*0.5f,  -h/2,  r*0.866f}, .normal = {-0.433f, 0.5f,  0.75f}, .uv = {1.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = { 0.0f,     h/2, 0.0f}, .normal = {-0.433f, 0.5f,  0.75f}, .uv = {0.5f, 1.0f}, .color = 0xFFFFFFFF },
		// Back right face
		{ .position = {-r*0.5f,  -h/2,  r*0.866f}, .normal = {-0.433f, 0.5f, -0.75f}, .uv = {0.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = {-r*0.5f,  -h/2, -r*0.866f}, .normal = {-0.433f, 0.5f, -0.75f}, .uv = {1.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = { 0.0f,     h/2, 0.0f}, .normal = {-0.433f, 0.5f, -0.75f}, .uv = {0.5f, 1.0f}, .color = 0xFFFFFFFF },
	};
	uint16_t pyramid_indices[] = {
		// Base
		0, 2, 1,
		0, 3, 2,
		0, 1, 3,
		// Sides
		5, 6, 7,    // Front right
		8, 9, 10,   // Back left
		11, 12, 13, // Back right
	};
	skr_mesh_create(&su_vertex_type_pnuc, skr_index_fmt_u16, pyramid_vertices, 14, pyramid_indices, 18, &scene->pyramid_mesh);
	skr_mesh_set_name(&scene->pyramid_mesh, "tetrahedron");

	// Load shader
	scene->shader = su_shader_load("shaders/orbital_particles.hlsl.sks", "orbital_particles_shader");
	skr_material_create((skr_material_info_t){
		.shader       = &scene->shader,
		.cull         = skr_cull_back,
		.write_mask   = skr_write_default,
		.depth_test   = skr_compare_less,
	}, &scene->material);

	// Create white 1x1 texture
	scene->white_texture = su_tex_create_solid_color(0xFFFFFFFF);
	skr_tex_set_name(&scene->white_texture, "white_1x1");

	// Load compute shader
	scene->compute_shader = su_shader_load("shaders/orbital_particles_compute.hlsl.sks", NULL);
	skr_compute_create(&scene->compute_shader, &scene->compute_ping);
	skr_compute_create(&scene->compute_shader, &scene->compute_pong);

	// Initialize particles in a sphere
	particle_t* particles = malloc(PARTICLE_COUNT * sizeof(particle_t));
	for (int i = 0; i < PARTICLE_COUNT; i++) {
		float theta   = _hash_f(i, 0) * 3.14159f * 2.0f;
		float phi     = _hash_f(i, 1) * 3.14159f;
		float radius  = _hash_f(i, 2) * 5.0f + 1.0f;

		particles[i].velocity = HMM_V3(0, 0, 0);
		particles[i].position = HMM_V3(
			sinf(phi) * cosf(theta) * radius,
			sinf(phi) * sinf(theta) * radius,
			cosf(phi) * radius
		);
	}

	// Create particle buffers for ping-pong compute
	skr_buffer_create(particles, PARTICLE_COUNT, sizeof(particle_t), skr_buffer_type_storage, skr_use_compute_readwrite, &scene->particle_buffer_a);
	skr_buffer_create(particles, PARTICLE_COUNT, sizeof(particle_t), skr_buffer_type_storage, skr_use_compute_readwrite, &scene->particle_buffer_b);
	free(particles);

	// Create compute params buffer
	typedef struct {
		float    time;
		float    delta_time;
		float    damping;
		float    max_speed;
		float    strength;
		uint32_t particle_count;
	} compute_params_t;

	compute_params_t compute_params = {
		.time           = 0.0f,
		.delta_time     = 0.0f,
		.damping        = 0.98f,
		.max_speed      = 5.0f,
		.strength       = 2.0f,
		.particle_count = PARTICLE_COUNT
	};
	skr_buffer_create(&compute_params, 1, sizeof(compute_params_t), skr_buffer_type_constant, skr_use_dynamic, &scene->compute_params_buffer);

	// Set up compute bindings
	skr_compute_set_buffer(&scene->compute_ping, "input",   &scene->particle_buffer_a);
	skr_compute_set_buffer(&scene->compute_ping, "output",  &scene->particle_buffer_b);
	skr_compute_set_buffer(&scene->compute_ping, "$Global", &scene->compute_params_buffer);

	skr_compute_set_buffer(&scene->compute_pong, "input",   &scene->particle_buffer_b);
	skr_compute_set_buffer(&scene->compute_pong, "output",  &scene->particle_buffer_a);
	skr_compute_set_buffer(&scene->compute_pong, "$Global", &scene->compute_params_buffer);

	scene->particle_params = (particle_params_t){
		.color_slow = HMM_V3(0.818f, 0.0100f, 0.0177f),  // Red (sRGB 0.92, 0.1, 0.14 -> linear)
		.max_speed  = 5.0f,
		.color_fast = HMM_V3(0.955f, 0.758f, 0.0177f),   // Yellow (sRGB 0.98, 0.89, 0.14 -> linear)
		._pad       = 0.0f
	};

	return (scene_t*)scene;
}

static void _scene_orbital_particles_destroy(scene_t* base) {
	scene_orbital_particles_t* scene = (scene_orbital_particles_t*)base;

	skr_mesh_destroy(&scene->pyramid_mesh);
	skr_material_destroy(&scene->material);
	skr_compute_destroy(&scene->compute_ping);
	skr_compute_destroy(&scene->compute_pong);
	skr_shader_destroy(&scene->compute_shader);
	skr_shader_destroy(&scene->shader);
	skr_tex_destroy(&scene->white_texture);
	skr_buffer_destroy(&scene->particle_buffer_a);
	skr_buffer_destroy(&scene->particle_buffer_b);
	skr_buffer_destroy(&scene->compute_params_buffer);

	free(scene);
}

static void _scene_orbital_particles_update(scene_t* base, float delta_time) {
	scene_orbital_particles_t* scene = (scene_orbital_particles_t*)base;
	scene->time += delta_time;

	// Update compute params
	typedef struct {
		float    time;
		float    delta_time;
		float    damping;
		float    max_speed;
		float    strength;
		uint32_t particle_count;
	} compute_params_t;

	compute_params_t params = {
		.time           = scene->time,
		.delta_time     = delta_time,
		.damping        = 0.98f,
		.max_speed      = 5.0f,
		.strength       = 4.0f,
		.particle_count = PARTICLE_COUNT
	};
	skr_buffer_set(&scene->compute_params_buffer, &params, sizeof(compute_params_t));

	// Execute compute shader to update particles on GPU
	skr_compute_t* current = (scene->compute_iteration % 2 == 0) ? &scene->compute_ping : &scene->compute_pong;
	// Dispatch 500k particles / 256 threads per group = 1954 groups (rounded up)
	skr_compute_execute(current, (PARTICLE_COUNT + 255) / 256, 1, 1);
	scene->compute_iteration++;
}

static void _scene_orbital_particles_render(scene_t* base, int32_t width, int32_t height, HMM_Mat4 viewproj, skr_render_list_t* ref_render_list, app_system_buffer_t* ref_system_buffer) {
	scene_orbital_particles_t* scene = (scene_orbital_particles_t*)base;

	// Bind particle params buffer (colors) to slot 0
	skr_material_set_params(&scene->material, &scene->particle_params, sizeof(scene->particle_params));

	// Use particle buffer directly - no CPU roundtrip needed!
	// The shader reads directly from the GPU buffer at slot 3
	skr_buffer_t* current_buffer = (scene->compute_iteration % 2 == 0) ? &scene->particle_buffer_a : &scene->particle_buffer_b;
	skr_material_set_buffer(&scene->material, "particles", current_buffer);

	// Draw with no instance data - shader reads from buffer binding
	skr_render_list_add(ref_render_list, &scene->pyramid_mesh, &scene->material, NULL, 0, PARTICLE_COUNT);
}

const scene_vtable_t scene_orbital_particles_vtable = {
	.name       = "Orbital Particles",
	.create     = _scene_orbital_particles_create,
	.destroy    = _scene_orbital_particles_destroy,
	.update     = _scene_orbital_particles_update,
	.render     = _scene_orbital_particles_render,
	.get_camera = NULL,
};
