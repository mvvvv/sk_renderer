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

// Shadow mapping scene - demonstrates basic single-cascade shadow mapping
typedef struct {
	scene_t            base;
	skr_render_list_t  render_list;
	skr_render_list_t  shadow_list;

	// Shadow map rendering
	skr_tex_t      shadow_map;
	skr_shader_t   shadow_caster_shader;
	skr_material_t shadow_caster_material;

	// Scene rendering
	skr_mesh_t     cube_mesh;
	skr_mesh_t     floor_mesh;
	skr_mesh_t     light_sphere_mesh;
	skr_mesh_t     light_ray_mesh;
	skr_shader_t   shadow_receiver_shader;
	skr_material_t cube_material;
	skr_material_t floor_material;
	skr_material_t light_material;
	skr_tex_t      checkerboard_texture;
	skr_tex_t      white_texture;
	skr_tex_t      yellow_texture;

	// Shadow buffer (sent to shader as constants)
	skr_buffer_t   shadow_buffer;

	float rotation;
	HMM_Vec3 light_dir;
} scene_shadows_t;

// Shadow buffer structure - matches the HLSL cbuffer
typedef struct {
	HMM_Mat4 shadow_transform;       // Transforms world space to shadow map UV space
	HMM_Vec3 light_direction;
	float    shadow_bias;
	HMM_Vec3 light_color;
	float    shadow_pixel_size;
} shadow_buffer_data_t;

// Configuration constants
static const float    SHADOW_SCENE_SIZE     = 10.0f;
static const float    SHADOW_MAP_SIZE       = 15.0f;
static const int32_t  SHADOW_MAP_RESOLUTION = 1024;
static const float    SHADOW_MAP_NEAR_CLIP  = 0.01f;
static const float    SHADOW_MAP_FAR_CLIP   = 30.0f;

// Quantize light position to avoid shadow shimmering
static HMM_Vec3 _quantize_light_pos(HMM_Vec3 pos, HMM_Mat4 view_matrix, float texel_size) {
	// Transform position to light view space
	HMM_Vec4 view_pos = HMM_MulM4V4(view_matrix, HMM_V4V(pos, 1.0f));

	// Quantize x and y to texel grid
	view_pos.X = roundf(view_pos.X / texel_size) * texel_size;
	view_pos.Y = roundf(view_pos.Y / texel_size) * texel_size;

	// Transform back to world space
	HMM_Mat4 view_inv = HMM_InvGeneralM4(view_matrix);
	HMM_Vec4 world_pos = HMM_MulM4V4(view_inv, view_pos);

	return HMM_V3(world_pos.X, world_pos.Y, world_pos.Z);
}

static scene_t* _scene_shadows_create() {
	scene_shadows_t* scene = calloc(1, sizeof(scene_shadows_t));
	if (!scene) return NULL;

	scene->base.size  = sizeof(scene_shadows_t);
	scene->rotation   = 0.0f;
	scene->light_dir  = HMM_NormV3(HMM_V3(1, -1, 0));
	scene->render_list = skr_render_list_create();
	scene->shadow_list = skr_render_list_create();

	// Create shadow map (depth texture)
	scene->shadow_map = skr_tex_create(
		skr_tex_fmt_depth16,
		skr_tex_flags_writeable | skr_tex_flags_readable,
		(skr_tex_sampler_t){
			.sample         = skr_tex_sample_linear,
			.address        = skr_tex_address_clamp,
			.sample_compare = skr_compare_less_or_eq,
			.anisotropy     = 1,
		},
		(skr_vec3i_t){SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION, 1},
		1, 0, NULL
	);
	skr_tex_set_name(&scene->shadow_map, "shadow_map");

	// Create cube mesh
	skr_vec4_t cube_colors[6] = {
		{0.8f, 0.3f, 0.3f, 1.0f},  // Front: Red
		{0.3f, 0.8f, 0.3f, 1.0f},  // Back: Green
		{0.3f, 0.3f, 0.8f, 1.0f},  // Top: Blue
		{0.8f, 0.8f, 0.3f, 1.0f},  // Bottom: Yellow
		{0.8f, 0.3f, 0.8f, 1.0f},  // Right: Magenta
		{0.3f, 0.8f, 0.8f, 1.0f},  // Left: Cyan
	};
	scene->cube_mesh = skr_mesh_create_cube(1.0f, cube_colors);
	skr_mesh_set_name(&scene->cube_mesh, "shadow_cube");

	// Create floor mesh (large quad on XZ plane)
	skr_vec4_t white = {1.0f, 1.0f, 1.0f, 1.0f};
	skr_vec3_t normal_up = {0, 1, 0};
	scene->floor_mesh = skr_mesh_create_quad(SHADOW_SCENE_SIZE, SHADOW_SCENE_SIZE, normal_up, false, white);
	skr_mesh_set_name(&scene->floor_mesh, "shadow_floor");

	// Load shadow caster shader
	void*  shader_data = NULL;
	size_t shader_size = 0;
	if (app_read_file("shaders/shadow_caster.hlsl.sks", &shader_data, &shader_size)) {
		scene->shadow_caster_shader = skr_shader_create(shader_data, shader_size);
		skr_shader_set_name(&scene->shadow_caster_shader, "shadow_caster");
		free(shader_data);

		if (skr_shader_is_valid(&scene->shadow_caster_shader)) {
			scene->shadow_caster_material = skr_material_create((skr_material_info_t){
				.shader     = &scene->shadow_caster_shader,
				.write_mask = skr_write_depth,
				.depth_test = skr_compare_less_or_eq,
			});
		}
	}

	// Load shadow receiver shader
	if (app_read_file("shaders/shadow_receiver.hlsl.sks", &shader_data, &shader_size)) {
		scene->shadow_receiver_shader = skr_shader_create(shader_data, shader_size);
		skr_shader_set_name(&scene->shadow_receiver_shader, "shadow_receiver");
		free(shader_data);

		if (skr_shader_is_valid(&scene->shadow_receiver_shader)) {
			scene->cube_material = skr_material_create((skr_material_info_t){
				.shader     = &scene->shadow_receiver_shader,
				.write_mask = skr_write_default,
				.depth_test = skr_compare_less,
			});

			scene->floor_material = skr_material_create((skr_material_info_t){
				.shader     = &scene->shadow_receiver_shader,
				.write_mask = skr_write_default,
				.depth_test = skr_compare_less,
			});

			scene->light_material = skr_material_create((skr_material_info_t){
				.shader     = &scene->shadow_receiver_shader,
				.write_mask = skr_write_default,
				.depth_test = skr_compare_less,
			});
		}
	}

	// Create light visualization meshes
	skr_vec4_t yellow = {1.0f, 1.0f, 0.0f, 1.0f};
	scene->light_sphere_mesh = skr_mesh_create_sphere(16, 12, 0.5f, yellow);
	skr_mesh_set_name(&scene->light_sphere_mesh, "light_sphere");

	// Create thin cube for light ray direction (1 unit long, very thin)
	skr_vec4_t light_ray_colors[6];
	for (int i = 0; i < 6; i++) light_ray_colors[i] = yellow;
	scene->light_ray_mesh = skr_mesh_create_cube(1.0f, light_ray_colors);
	skr_mesh_set_name(&scene->light_ray_mesh, "light_ray");

	// Create textures
	scene->checkerboard_texture = skr_tex_create_checkerboard(512, 32, 0xFFFFFFFF, 0xFF888888, true);
	skr_tex_set_name(&scene->checkerboard_texture, "floor_checker");
	scene->white_texture = skr_tex_create_solid_color(0xFFFFFFFF);
	skr_tex_set_name(&scene->white_texture, "white_1x1");
	scene->yellow_texture = skr_tex_create_solid_color(0xFFFFFF00);
	skr_tex_set_name(&scene->yellow_texture, "yellow_1x1");

	// Bind textures to materials (shadow map will be bound globally per frame)
	if (skr_material_is_valid(&scene->cube_material)) {
		skr_material_set_tex(&scene->cube_material, 0, &scene->white_texture);
	}
	if (skr_material_is_valid(&scene->floor_material)) {
		skr_material_set_tex(&scene->floor_material, 0, &scene->checkerboard_texture);
	}
	if (skr_material_is_valid(&scene->light_material)) {
		skr_material_set_tex(&scene->light_material, 0, &scene->yellow_texture);
	}

	// Create shadow buffer (constant buffer for shadow parameters)
	shadow_buffer_data_t shadow_data = {0};
	scene->shadow_buffer = skr_buffer_create(&shadow_data, sizeof(shadow_buffer_data_t), 1, skr_buffer_type_constant, skr_use_dynamic);
	skr_buffer_set_name(&scene->shadow_buffer, "shadow_constants");

	return (scene_t*)scene;
}

static void _scene_shadows_destroy(scene_t* base) {
	scene_shadows_t* scene = (scene_shadows_t*)base;

	skr_render_list_destroy(&scene->render_list);
	skr_render_list_destroy(&scene->shadow_list);
	skr_mesh_destroy(&scene->cube_mesh);
	skr_mesh_destroy(&scene->floor_mesh);
	skr_mesh_destroy(&scene->light_sphere_mesh);
	skr_mesh_destroy(&scene->light_ray_mesh);
	skr_material_destroy(&scene->shadow_caster_material);
	skr_material_destroy(&scene->cube_material);
	skr_material_destroy(&scene->floor_material);
	skr_material_destroy(&scene->light_material);
	skr_shader_destroy(&scene->shadow_caster_shader);
	skr_shader_destroy(&scene->shadow_receiver_shader);
	skr_tex_destroy(&scene->shadow_map);
	skr_tex_destroy(&scene->checkerboard_texture);
	skr_tex_destroy(&scene->white_texture);
	skr_tex_destroy(&scene->yellow_texture);
	skr_buffer_destroy(&scene->shadow_buffer);

	free(scene);
}

static void _scene_shadows_update(scene_t* base, float delta_time) {
	scene_shadows_t* scene = (scene_shadows_t*)base;
	scene->rotation += delta_time * 0.5f;
	scene->light_dir = HMM_NormV3(HMM_V3(cosf(scene->rotation), -1, sinf(scene->rotation)));
}

static void _scene_shadows_render(scene_t* base, int32_t width, int32_t height, HMM_Mat4 viewproj, skr_render_list_t* ref_render_list, app_system_buffer_t* ref_system_buffer) {
	scene_shadows_t* scene = (scene_shadows_t*)base;

	typedef struct { HMM_Mat4 world; } instance_data_t;

	// Setup shadow map matrices
	HMM_Vec3 scene_center = HMM_V3(0, 0, 0);
	float    texel_size = SHADOW_MAP_SIZE / SHADOW_MAP_RESOLUTION;

	// Initial light position (before quantization)
	HMM_Vec3 light_pos_initial = HMM_AddV3(scene_center, HMM_MulV3F(scene->light_dir, -15.0f));

	// Create preliminary shadow view matrix for quantization
	HMM_Mat4 shadow_view_prelim = HMM_LookAt_RH(light_pos_initial, HMM_AddV3(light_pos_initial, scene->light_dir), HMM_V3(0, 1, 0));

	// Quantize light position to reduce shadow shimmering
	HMM_Vec3 light_pos = _quantize_light_pos(light_pos_initial, shadow_view_prelim, texel_size);

	// Create final shadow map view/projection matrices
	HMM_Mat4 shadow_view = HMM_LookAt_RH(light_pos, HMM_AddV3(light_pos, scene->light_dir), HMM_V3(0, 1, 0));
	HMM_Mat4 shadow_proj = HMM_Orthographic_RH_ZO(
		-SHADOW_MAP_SIZE * 0.5f, SHADOW_MAP_SIZE * 0.5f,
		-SHADOW_MAP_SIZE * 0.5f, SHADOW_MAP_SIZE * 0.5f,
		SHADOW_MAP_NEAR_CLIP, SHADOW_MAP_FAR_CLIP
	);

	// Compute shadow transform matrix (world space -> shadow clip space)
	HMM_Mat4 shadow_transform = HMM_MulM4(shadow_proj, shadow_view);

	// Update shadow buffer
	float slope_scale = fmaxf((SHADOW_MAP_FAR_CLIP - SHADOW_MAP_NEAR_CLIP) / 65536.0f, texel_size);
	shadow_buffer_data_t shadow_data = {
		.shadow_transform  = HMM_Transpose(shadow_transform),
		.light_direction   = HMM_MulV3F(scene->light_dir, -1.0f),
		.shadow_bias       = slope_scale * 2.0f,
		.light_color       = HMM_V3(1, 1, 1),
		.shadow_pixel_size = 1.0f / SHADOW_MAP_RESOLUTION,
	};
	skr_buffer_set(&scene->shadow_buffer, &shadow_data, sizeof(shadow_buffer_data_t));

	// Build instance data for scene objects
	const int32_t cube_count = 20;
	instance_data_t cube_instances[20];

	// Generate random cubes (using deterministic random seed)
	for (int32_t i = 0; i < cube_count; i++) {
		float rand_seed = (float)i / (float)cube_count;
		float x = (skr_hash_f(i * 3 + 0, 1) - 0.5f) * (SHADOW_SCENE_SIZE - 1.0f);
		float y = (skr_hash_f(i * 3 + 1, 1) - 0.5f) * (SHADOW_SCENE_SIZE - 1.0f);
		float size_x = 0.2f + skr_hash_f(i * 3 + 2, 1) * 0.4f;
		float size_y = 0.3f + skr_hash_f(i * 3 + 2, 1) * 1.5f;
		float size_z = 0.2f + skr_hash_f(i * 3 + 2, 1) * 0.4f;

		HMM_Mat4 transform = HMM_MulM4(
			HMM_Translate(HMM_V3(x, 0.01f + size_y * 0.5f, y)),
			HMM_Scale(HMM_V3(size_x, size_y, size_z))
		);
		cube_instances[i].world = HMM_Transpose(transform);
	}

	// Floor instance
	instance_data_t floor_instance;
	floor_instance.world = HMM_Transpose(HMM_M4D(1.0f));

	// Create system buffer for shadow pass (orthographic projection)
	app_system_buffer_t shadow_sys_buffer = {0};
	shadow_sys_buffer.view_count = 1;

	HMM_Mat4 shadow_view_t    = HMM_Transpose(shadow_view);
	HMM_Mat4 shadow_proj_t    = HMM_Transpose(shadow_proj);
	HMM_Mat4 shadow_viewproj  = HMM_MulM4(shadow_proj, shadow_view);
	HMM_Mat4 shadow_viewproj_t = HMM_Transpose(shadow_viewproj);

	memcpy(shadow_sys_buffer.view      [0], &shadow_view_t,     sizeof(float) * 16);
	memcpy(shadow_sys_buffer.projection[0], &shadow_proj_t,     sizeof(float) * 16);
	memcpy(shadow_sys_buffer.viewproj  [0], &shadow_viewproj_t, sizeof(float) * 16);

	// Clear global texture/constants that shadow caster doesn't use
	skr_renderer_set_global_constants(13, NULL);
	skr_renderer_set_global_texture(14, NULL);

	// Render shadow map (depth-only pass)
	skr_renderer_begin_pass(NULL, &scene->shadow_map, NULL, skr_clear_depth, (skr_vec4_t){0, 0, 0, 0}, 1.0f, 0);
	skr_renderer_set_viewport((skr_rect_t ){0, 0, (float)SHADOW_MAP_RESOLUTION, (float)SHADOW_MAP_RESOLUTION});
	skr_renderer_set_scissor ((skr_recti_t){0, 0, SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION});

	skr_render_list_add(&scene->shadow_list, &scene->cube_mesh,  &scene->shadow_caster_material, cube_instances, sizeof(instance_data_t), cube_count);
	skr_render_list_add(&scene->shadow_list, &scene->floor_mesh, &scene->shadow_caster_material, &floor_instance, sizeof(instance_data_t), 1);
	skr_renderer_draw(&scene->shadow_list, &shadow_sys_buffer, sizeof(app_system_buffer_t), 1);
	skr_render_list_clear(&scene->shadow_list);
	skr_renderer_end_pass();

	// Bind shadow buffer and shadow map globally (b13 for constants, t14 for texture to avoid slot conflicts)
	skr_renderer_set_global_constants(13, &scene->shadow_buffer);
	skr_renderer_set_global_texture(14, &scene->shadow_map);

	// Create light visualization instances
	instance_data_t light_sphere_instance;
	light_sphere_instance.world = HMM_Transpose(HMM_Translate(light_pos));

	// Create light ray instance (thin cube pointing in light direction)
	HMM_Vec3 ray_start = light_pos;
	HMM_Vec3 ray_end = HMM_AddV3(light_pos, HMM_MulV3F(scene->light_dir, 20.0f));
	HMM_Vec3 ray_mid = HMM_MulV3F(HMM_AddV3(ray_start, ray_end), 0.5f);

	// Calculate rotation to align cube with light direction
	HMM_Vec3 up = HMM_V3(0, 1, 0);
	HMM_Vec3 right = HMM_NormV3(HMM_Cross(up, scene->light_dir));
	if (HMM_LenV3(right) < 0.001f) {
		// Light direction is parallel to up, use different axis
		right = HMM_V3(1, 0, 0);
	}
	up = HMM_NormV3(HMM_Cross(scene->light_dir, right));


	// Render scene with shadows to main render target
	skr_render_list_add(ref_render_list, &scene->floor_mesh, &scene->floor_material, &floor_instance, sizeof(instance_data_t), 1);
	skr_render_list_add(ref_render_list, &scene->cube_mesh,  &scene->cube_material,  cube_instances,  sizeof(instance_data_t), cube_count);
}

const scene_vtable_t scene_shadows_vtable = {
	.name       = "Shadow Mapping",
	.create     = _scene_shadows_create,
	.destroy    = _scene_shadows_destroy,
	.update     = _scene_shadows_update,
	.render     = _scene_shadows_render,
	.get_camera = NULL,
};
