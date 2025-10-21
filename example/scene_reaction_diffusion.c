#include "scene.h"
#include "scene_util.h"
#include "app.h"

#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#include <stdlib.h>
#include <string.h>

// Reaction-diffusion scene - displays compute shader simulation on a quad
typedef struct {
	scene_t       base;

	skr_mesh_t     quad_mesh;
	skr_shader_t   shader;
	skr_shader_t   compute_sh;
	skr_material_t quad_material;
	skr_compute_t  compute_ping;
	skr_compute_t  compute_pong;
	skr_buffer_t   compute_buffer_a;
	skr_buffer_t   compute_buffer_b;
	skr_buffer_t   compute_params_buffer;
	skr_tex_t      compute_output;

	int32_t sim_size;
	int32_t compute_iteration;
	float   rotation;
} scene_reaction_diffusion_t;

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

static scene_t* _scene_reaction_diffusion_create() {
	scene_reaction_diffusion_t* scene = calloc(1, sizeof(scene_reaction_diffusion_t));
	if (!scene) return NULL;

	scene->base.size         = sizeof(scene_reaction_diffusion_t);
	scene->sim_size          = 512;
	scene->compute_iteration = 0;
	scene->rotation          = 0.0f;

	// Create double-sided quad mesh (front face + back face with flipped normals)
	skr_vertex_pnuc_t quad_vertices[] = {
		// Front face (Z+)
		{ .position = {-0.7f, -0.7f, 0.0f, 1.0f}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {0.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
		{ .position = { 0.7f, -0.7f, 0.0f, 1.0f}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
		{ .position = { 0.7f,  0.7f, 0.0f, 1.0f}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {1.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
		{ .position = {-0.7f,  0.7f, 0.0f, 1.0f}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {0.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
		// Back face (Z-) - same positions, flipped normals and winding
		{ .position = {-0.7f, -0.7f, 0.0f, 1.0f}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {0.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
		{ .position = { 0.7f, -0.7f, 0.0f, 1.0f}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
		{ .position = { 0.7f,  0.7f, 0.0f, 1.0f}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {1.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
		{ .position = {-0.7f,  0.7f, 0.0f, 1.0f}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {0.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
	};
	uint16_t quad_indices[] = {
		0, 1, 2,  2, 3, 0,  // Front face
		5, 4, 7,  7, 6, 5,  // Back face (flipped winding)
	};
	scene->quad_mesh = skr_mesh_create(&skr_vertex_type_pnuc, skr_index_fmt_u16, quad_vertices, 8, quad_indices, 12);
	skr_mesh_set_name(&scene->quad_mesh, "quad");

	// Load shaders
	void*  shader_data = NULL;
	size_t shader_size = 0;
	if (app_read_file("shaders/test.hlsl.sks", &shader_data, &shader_size)) {
		scene->shader = skr_shader_create(shader_data, shader_size);
		skr_shader_set_name(&scene->shader, "main_shader");
		free(shader_data);

		if (skr_shader_is_valid(&scene->shader)) {
			scene->quad_material = skr_material_create((skr_material_info_t){
				.shader       = &scene->shader,
				.cull         = skr_cull_back,
				.write_mask   = skr_write_r | skr_write_g | skr_write_b | skr_write_a | skr_write_depth,
				.depth_test   = skr_compare_less,
			});
		}
	}

	// Load compute shader
	void* compute_data = NULL;
	size_t compute_size = 0;
	if (app_read_file("shaders/compute_test.hlsl.sks", &compute_data, &compute_size)) {
		scene->compute_sh = skr_shader_create(compute_data, compute_size);
		free(compute_data);

		if (skr_shader_is_valid(&scene->compute_sh)) {
			scene->compute_ping = skr_compute_create(&scene->compute_sh);
			scene->compute_pong = skr_compute_create(&scene->compute_sh);
		}
	}

	// Create compute resources
	typedef struct { float x, y; } float2;
	float2* initial_data = malloc(scene->sim_size * scene->sim_size * sizeof(float2));
	for (int y = 0; y < scene->sim_size; y++) {
		for (int x = 0; x < scene->sim_size; x++) {
			float r = _hash_f(1, (uint32_t)((x/16)*13+(y/16)*127));
			initial_data[x + y * scene->sim_size].x = r;
			initial_data[x + y * scene->sim_size].y = 1.0f - r;
		}
	}

	scene->compute_buffer_a = skr_buffer_create(initial_data, scene->sim_size * scene->sim_size, sizeof(float2),
		skr_buffer_type_storage, skr_use_compute_readwrite);
	scene->compute_buffer_b = skr_buffer_create(initial_data, scene->sim_size * scene->sim_size, sizeof(float2),
		skr_buffer_type_storage, skr_use_compute_readwrite);
	free(initial_data);

	skr_tex_sampler_t default_sampler = { .sample = skr_tex_sample_linear, .address = skr_tex_address_clamp };
	scene->compute_output = skr_tex_create(skr_tex_fmt_rgba128,
		skr_tex_flags_readable | skr_tex_flags_compute,
		default_sampler,
		(skr_vec3i_t){scene->sim_size, scene->sim_size, 1}, 1, 1, NULL);

	// Create compute params buffer
	typedef struct {
		float    feed;
		float    kill;
		float    diffuseA;
		float    diffuseB;
		float    timestep;
		uint32_t size;
	} compute_params_t;

	compute_params_t compute_params = {
		.feed     = 0.042f,
		.kill     = 0.059f,
		.diffuseA = 0.2097f,
		.diffuseB = 0.105f,
		.timestep = 0.8f,
		.size     = scene->sim_size
	};
	scene->compute_params_buffer = skr_buffer_create(&compute_params, 1, sizeof(compute_params_t),
		skr_buffer_type_constant, skr_use_dynamic);

	// Set up compute bindings
	if (skr_compute_is_valid(&scene->compute_ping) && skr_compute_is_valid(&scene->compute_pong)) {
		skr_bind_t input_bind  = skr_compute_get_bind(&scene->compute_ping, "input");
		skr_bind_t output_bind = skr_compute_get_bind(&scene->compute_ping, "output");
		skr_bind_t params_bind = skr_compute_get_bind(&scene->compute_ping, "$Global");
		skr_bind_t tex_bind    = skr_compute_get_bind(&scene->compute_ping, "out_tex");

		skr_compute_set_buffer(&scene->compute_ping, input_bind.slot,  &scene->compute_buffer_a);
		skr_compute_set_buffer(&scene->compute_ping, output_bind.slot, &scene->compute_buffer_b);
		skr_compute_set_buffer(&scene->compute_ping, params_bind.slot, &scene->compute_params_buffer);
		skr_compute_set_tex   (&scene->compute_ping, tex_bind.slot,    &scene->compute_output);

		skr_compute_set_buffer(&scene->compute_pong, input_bind.slot,  &scene->compute_buffer_b);
		skr_compute_set_buffer(&scene->compute_pong, output_bind.slot, &scene->compute_buffer_a);
		skr_compute_set_buffer(&scene->compute_pong, params_bind.slot, &scene->compute_params_buffer);
		skr_compute_set_tex   (&scene->compute_pong, tex_bind.slot,    &scene->compute_output);
	}

	// Bind texture to material
	if (skr_material_is_valid(&scene->quad_material)) {
		skr_material_set_tex(&scene->quad_material, 0, &scene->compute_output);
	}

	return (scene_t*)scene;
}

static void _scene_reaction_diffusion_destroy(scene_t* base) {
	scene_reaction_diffusion_t* scene = (scene_reaction_diffusion_t*)base;

	skr_mesh_destroy(&scene->quad_mesh);
	skr_material_destroy(&scene->quad_material);
	skr_compute_destroy(&scene->compute_ping);
	skr_compute_destroy(&scene->compute_pong);
	skr_shader_destroy(&scene->compute_sh);
	skr_shader_destroy(&scene->shader);
	skr_tex_destroy(&scene->compute_output);
	skr_buffer_destroy(&scene->compute_buffer_a);
	skr_buffer_destroy(&scene->compute_buffer_b);
	skr_buffer_destroy(&scene->compute_params_buffer);

	free(scene);
}

static void _scene_reaction_diffusion_update(scene_t* base, float delta_time) {
	scene_reaction_diffusion_t* scene = (scene_reaction_diffusion_t*)base;
	scene->rotation += delta_time;

	// Execute compute shader
	if (skr_compute_is_valid(&scene->compute_ping) && skr_compute_is_valid(&scene->compute_pong)) {
		for (int c = 0; c < 2; c++) {
			skr_compute_t* current = (scene->compute_iteration % 2 == 0) ? &scene->compute_ping : &scene->compute_pong;
			skr_compute_execute(current, scene->sim_size / 8, scene->sim_size / 8, 1);
			scene->compute_iteration++;
		}
	}
}

static void _scene_reaction_diffusion_render(scene_t* base, int32_t width, int32_t height, HMM_Mat4 viewproj, skr_render_list_t* ref_render_list, app_system_buffer_t* ref_system_buffer) {
	scene_reaction_diffusion_t* scene = (scene_reaction_diffusion_t*)base;

	// Build instance data for quad
	typedef struct { HMM_Mat4 world; } instance_data_t;
	instance_data_t quad_instance;
	HMM_Mat4 quad_transform = HMM_MulM4(
		HMM_Scale(HMM_V3(6.0f, 6.0f, 6.0f)),
		HMM_Rotate_RH(-scene->rotation, HMM_V3(0.0f, 1.0f, 0.0f))
	);
	quad_instance.world = HMM_Transpose(quad_transform);

	// Add to render list
	skr_render_list_add(ref_render_list, &scene->quad_mesh, &scene->quad_material, &quad_instance, sizeof(instance_data_t), 1);
}

const scene_vtable_t scene_reaction_diffusion_vtable = {
	.name       = "Reaction-Diffusion Simulation",
	.create     = _scene_reaction_diffusion_create,
	.destroy    = _scene_reaction_diffusion_destroy,
	.update     = _scene_reaction_diffusion_update,
	.render     = _scene_reaction_diffusion_render,
	.get_camera = NULL,
};
