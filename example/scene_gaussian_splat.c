// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Nick Klingensmith

#include "scene.h"
#include "tools/scene_util.h"
#include "app.h"
#include "tools/float_math.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#define MICRO_PLY_IMPL
#include "tools/micro_ply.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

// Gaussian splat data structure (must match shader's structured buffer layout)
// HLSL arrays of float3 pad each element to 16 bytes!
typedef struct {
	float3   position;
	float    opacity;
	float3   sh_dc;      // f_dc_0, f_dc_1, f_dc_2
	float    _pad1;      // Padding to align scale
	float3   scale;      // scale_0, scale_1, scale_2
	float    _pad2;      // Padding to align rotation
	float4   rotation;   // rot_0, rot_1, rot_2, rot_3
	float4   sh_rest[15]; // f_rest_0..44 grouped by channel (HLSL pads float3[] to float4[])
} gaussian_splat_t;

// Scene state
typedef struct {
	scene_t         base;

	// Splat data
	uint32_t        splat_count;
	skr_buffer_t    splat_buffer;
	skr_buffer_t    sort_index_buffer;    // indices_a
	skr_buffer_t    sort_depth_buffer;    // depths_a
	skr_buffer_t    sort_index_buffer_b;  // indices_b (radix ping-pong)
	skr_buffer_t    sort_depth_buffer_b;  // depths_b (radix ping-pong)
	skr_buffer_t    histogram_buffer;     // radix histogram (65536 bins)
	skr_buffer_t    ranks_buffer;         // per-element rank within bin (for stable sort)

	// Rendering
	skr_mesh_t      quad_mesh;
	skr_shader_t    render_shader;
	skr_material_t  render_material;

	// Sorting compute (bitonic - legacy)
	skr_shader_t    sort_shader;
	skr_compute_t   sort_compute;

	// Radix sort compute
	skr_shader_t    radix_shader;
	skr_compute_t   radix_compute;
	bool            use_radix_sort;

	// UI controls
	float           splat_scale;
	float           opacity_scale;
	int32_t         sh_degree;
	float           max_radius;   // Max splat radius in pixels (0 = unlimited)
	bool            enable_sort;
	bool            flip_y;       // Flip Y axis (for assets with different coordinate conventions)
	char*           ply_path;

	// Sort state (incremental sorting across frames)
	uint32_t        sort_stage;
	uint32_t        sort_step;
	bool            sort_depths_computed;
	bool            initial_sort_complete;  // True after first full bitonic sort
	uint32_t        oddeven_phase;          // 0 = even pairs, 1 = odd pairs
	uint32_t        radix_pass;             // Current radix sort pass (0-4)
	float4x4        last_sorted_view_mat;   // View matrix from last completed sort
	bool            needs_resort;           // True when camera moved and re-sort needed

	// Camera state (arc-ball style)
	float           cam_yaw;
	float           cam_pitch;
	float           cam_distance;
	float3          cam_target;
	float           cam_yaw_vel;
	float           cam_pitch_vel;
	float           cam_distance_vel;
	float3          cam_target_vel;

	float           time;
} scene_gaussian_splat_t;

// Load Gaussian splat data from PLY file
static bool _load_splat_ply(scene_gaussian_splat_t* scene, const char* filename) {
	void*  data = NULL;
	size_t size = 0;

	if (!su_file_read(filename, &data, &size)) {
		su_log(su_log_warning, "gaussian_splat: Failed to read PLY file: %s", filename);
		return false;
	}

	ply_file_t ply = {0};
	if (!ply_read(data, size, &ply)) {
		su_log(su_log_warning, "gaussian_splat: Failed to parse PLY file: %s", filename);
		free(data);
		return false;
	}

	// Find vertex count
	int32_t vertex_count = 0;
	for (int32_t i = 0; i < ply.count; i++) {
		if (strcmp(ply.elements[i].name, PLY_ELEMENT_VERTICES) == 0) {
			vertex_count = ply.elements[i].count;
			break;
		}
	}

	if (vertex_count == 0) {
		su_log(su_log_warning, "gaussian_splat: No vertices found in PLY file");
		ply_free(&ply);
		free(data);
		return false;
	}

	su_log(su_log_info, "gaussian_splat: Loading %d splats from %s (struct size: %zu bytes)",
	       vertex_count, filename, sizeof(gaussian_splat_t));

	// Allocate splat data
	gaussian_splat_t* splats = calloc(vertex_count, sizeof(gaussian_splat_t));
	if (!splats) {
		ply_free(&ply);
		free(data);
		return false;
	}

	// Define the mapping from PLY properties to our splat structure
	float zero = 0.0f;
	float one  = 1.0f;

	// Position and basic properties
	ply_map_t map_basic[] = {
		{ PLY_PROP_POSITION_X, ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, position) + 0,  &zero },
		{ PLY_PROP_POSITION_Y, ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, position) + 4,  &zero },
		{ PLY_PROP_POSITION_Z, ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, position) + 8,  &zero },
		{ "opacity",           ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, opacity),       &zero },
		{ "f_dc_0",            ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, sh_dc) + 0,     &zero },
		{ "f_dc_1",            ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, sh_dc) + 4,     &zero },
		{ "f_dc_2",            ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, sh_dc) + 8,     &zero },
		{ "scale_0",           ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, scale) + 0,     &zero },
		{ "scale_1",           ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, scale) + 4,     &zero },
		{ "scale_2",           ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, scale) + 8,     &zero },
		{ "rot_0",             ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, rotation) + 0,  &one  },
		{ "rot_1",             ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, rotation) + 4,  &zero },
		{ "rot_2",             ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, rotation) + 8,  &zero },
		{ "rot_3",             ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, rotation) + 12, &zero },
	};

	// Convert basic properties
	void*   out_data  = NULL;
	int32_t out_count = 0;
	ply_convert(&ply, PLY_ELEMENT_VERTICES, map_basic, sizeof(map_basic) / sizeof(ply_map_t),
	            sizeof(gaussian_splat_t), &out_data, &out_count);

	if (out_data && out_count > 0) {
		memcpy(splats, out_data, out_count * sizeof(gaussian_splat_t));
		free(out_data);
	}

	// Now load the SH rest coefficients (f_rest_0 to f_rest_44)
	// These are stored as 45 floats, but we want to reorganize them into 15 float3s
	// PLY order: f_rest_0..14 (SH order 1,2,3 for R), f_rest_15..29 (G), f_rest_30..44 (B)
	// We need: sh_rest[0] = {f_rest_0, f_rest_15, f_rest_30}, etc.

	// Find the vertex element
	ply_element_t* vert_elem = NULL;
	for (int32_t i = 0; i < ply.count; i++) {
		if (strcmp(ply.elements[i].name, PLY_ELEMENT_VERTICES) == 0) {
			vert_elem = &ply.elements[i];
			break;
		}
	}

	if (vert_elem) {
		// Load f_rest coefficients one by one
		for (int32_t sh_idx = 0; sh_idx < 15; sh_idx++) {
			char name_r[16], name_g[16], name_b[16];
			snprintf(name_r, sizeof(name_r), "f_rest_%d", sh_idx);
			snprintf(name_g, sizeof(name_g), "f_rest_%d", sh_idx + 15);
			snprintf(name_b, sizeof(name_b), "f_rest_%d", sh_idx + 30);

			// sh_rest is float4[15] to match HLSL alignment (float3 arrays pad to 16 bytes/element)
			ply_map_t map_sh[] = {
				{ name_r, ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, sh_rest) + sh_idx * 16 + 0, &zero },
				{ name_g, ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, sh_rest) + sh_idx * 16 + 4, &zero },
				{ name_b, ply_prop_decimal, sizeof(float), offsetof(gaussian_splat_t, sh_rest) + sh_idx * 16 + 8, &zero },
			};

			void*   sh_data  = NULL;
			int32_t sh_count = 0;
			ply_convert(&ply, PLY_ELEMENT_VERTICES, map_sh, 3, sizeof(gaussian_splat_t), &sh_data, &sh_count);

			if (sh_data && sh_count > 0) {
				// Copy just the sh_rest data
				for (int32_t v = 0; v < sh_count && v < vertex_count; v++) {
					gaussian_splat_t* src = (gaussian_splat_t*)sh_data + v;
					splats[v].sh_rest[sh_idx] = src->sh_rest[sh_idx];
				}
				free(sh_data);
			}
		}
	}

	// Normalize quaternions and compute bounding box
	float3 bbox_min = { 1e10f,  1e10f,  1e10f};
	float3 bbox_max = {-1e10f, -1e10f, -1e10f};

	for (int32_t i = 0; i < vertex_count; i++) {
		// Apply coordinate transform for COLMAP/3DGS (Y-down, Z-forward) to sk_renderer (Y-up, Z-backward)
		// Position: flip Y and Z
		splats[i].position.y = -splats[i].position.y;
		splats[i].position.z = -splats[i].position.z;
		// Quaternion: for Y-Z flip, negate the y and z components
		// Storage: .x=w, .y=x, .z=y, .w=z
		splats[i].rotation.z = -splats[i].rotation.z;  // Negate y component
		splats[i].rotation.w = -splats[i].rotation.w;  // Negate z component

		// Normalize quaternion
		float4 q = splats[i].rotation;
		float len = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
		if (len > 0.0001f) {
			splats[i].rotation.x = q.x / len;
			splats[i].rotation.y = q.y / len;
			splats[i].rotation.z = q.z / len;
			splats[i].rotation.w = q.w / len;
		}

		// Update bounding box
		float3 p = splats[i].position;
		if (p.x < bbox_min.x) bbox_min.x = p.x;
		if (p.y < bbox_min.y) bbox_min.y = p.y;
		if (p.z < bbox_min.z) bbox_min.z = p.z;
		if (p.x > bbox_max.x) bbox_max.x = p.x;
		if (p.y > bbox_max.y) bbox_max.y = p.y;
		if (p.z > bbox_max.z) bbox_max.z = p.z;
	}

	// Set camera target to center of bounding box
	scene->cam_target = (float3){0,0,0};/* (float3){
		(bbox_min.x + bbox_max.x) * 0.5f,
		(bbox_min.y + bbox_max.y) * 0.5f,
		(bbox_min.z + bbox_max.z) * 0.5f
	};*/

	// Set camera distance based on bounding box size
	float3 bbox_size = {1,1,1};/* {
		bbox_max.x - bbox_min.x,
		bbox_max.y - bbox_min.y,
		bbox_max.z - bbox_min.z
	};*/
	float max_dim = fmaxf(fmaxf(bbox_size.x, bbox_size.y), bbox_size.z);
	scene->cam_distance = max_dim * 1.5f;

	su_log(su_log_info, "gaussian_splat: Bounds [%.2f,%.2f,%.2f] - [%.2f,%.2f,%.2f], size %.2f",
	       bbox_min.x, bbox_min.y, bbox_min.z,
	       bbox_max.x, bbox_max.y, bbox_max.z, max_dim);

	// Create GPU buffers
	scene->splat_count = vertex_count;

	skr_buffer_create(splats, vertex_count, sizeof(gaussian_splat_t),
	                  skr_buffer_type_storage, skr_use_compute_read, &scene->splat_buffer);
	skr_buffer_set_name(&scene->splat_buffer, "gaussian_splat_data");

	// Create sort buffers (primary A buffers)
	uint32_t* indices = malloc(vertex_count * sizeof(uint32_t));
	float*    depths  = malloc(vertex_count * sizeof(float));
	for (uint32_t i = 0; i < (uint32_t)vertex_count; i++) {
		indices[i] = i;
		depths[i]  = 0.0f;
	}

	skr_buffer_create(indices, vertex_count, sizeof(uint32_t),
	                  skr_buffer_type_storage, skr_use_compute_readwrite, &scene->sort_index_buffer);
	skr_buffer_set_name(&scene->sort_index_buffer, "gaussian_sort_indices_a");

	skr_buffer_create(depths, vertex_count, sizeof(float),
	                  skr_buffer_type_storage, skr_use_compute_readwrite, &scene->sort_depth_buffer);
	skr_buffer_set_name(&scene->sort_depth_buffer, "gaussian_sort_depths_a");

	// Create ping-pong B buffers for radix sort
	skr_buffer_create(indices, vertex_count, sizeof(uint32_t),
	                  skr_buffer_type_storage, skr_use_compute_readwrite, &scene->sort_index_buffer_b);
	skr_buffer_set_name(&scene->sort_index_buffer_b, "gaussian_sort_indices_b");

	skr_buffer_create(depths, vertex_count, sizeof(float),
	                  skr_buffer_type_storage, skr_use_compute_readwrite, &scene->sort_depth_buffer_b);
	skr_buffer_set_name(&scene->sort_depth_buffer_b, "gaussian_sort_depths_b");

	// Create histogram buffer for radix sort (65536 bins for high precision)
	uint32_t* histogram = calloc(65536, sizeof(uint32_t));
	skr_buffer_create(histogram, 65536, sizeof(uint32_t),
	                  skr_buffer_type_storage, skr_use_compute_readwrite, &scene->histogram_buffer);
	skr_buffer_set_name(&scene->histogram_buffer, "gaussian_radix_histogram");
	free(histogram);

	// Create ranks buffer for stable sort (per-element rank within its bin)
	uint32_t* ranks = calloc(vertex_count, sizeof(uint32_t));
	skr_buffer_create(ranks, vertex_count, sizeof(uint32_t),
	                  skr_buffer_type_storage, skr_use_compute_readwrite, &scene->ranks_buffer);
	skr_buffer_set_name(&scene->ranks_buffer, "gaussian_radix_ranks");
	free(ranks);

	free(indices);
	free(depths);
	free(splats);

	ply_free(&ply);
	free(data);

	return true;
}

static scene_t* _scene_gaussian_splat_create(void) {
	scene_gaussian_splat_t* scene = calloc(1, sizeof(scene_gaussian_splat_t));
	if (!scene) return NULL;

	scene->base.size = sizeof(scene_gaussian_splat_t);

	// Default parameters
	scene->splat_scale   = 1.0f;
	scene->opacity_scale = 1.0f;
	scene->sh_degree     = 3;
	scene->max_radius    = 256.0f;  // Cap splat size to prevent massive overdraw
	scene->enable_sort   = true;
	scene->use_radix_sort = true;   // Use radix sort by default (faster)
	scene->time          = 0.0f;

	// Camera defaults (will be updated when PLY loads)
	scene->cam_yaw      = 0.0f;
	scene->cam_pitch    = 0.0f;
	scene->cam_distance = 5.0f;
	scene->cam_target   = (float3){0, 0, 0};

	
	// Load PLY file
	scene->ply_path = strdup("/home/koujaku/Downloads/bicycle-road.ply");//"goldorak-ply.ply");
	if (!_load_splat_ply(scene, scene->ply_path)) {
		su_log(su_log_warning, "gaussian_splat: Failed to load default PLY, scene will be empty");
	}

	// Create a unit quad mesh for rendering splats
	// Each splat is rendered as a screen-aligned quad
	su_vertex_t quad_verts[] = {
		{ .position = {-1, -1, 0}, .normal = {0, 0, 1}, .uv = {0, 0}, .color = 0xFFFFFFFF },
		{ .position = { 1, -1, 0}, .normal = {0, 0, 1}, .uv = {1, 0}, .color = 0xFFFFFFFF },
		{ .position = { 1,  1, 0}, .normal = {0, 0, 1}, .uv = {1, 1}, .color = 0xFFFFFFFF },
		{ .position = {-1,  1, 0}, .normal = {0, 0, 1}, .uv = {0, 1}, .color = 0xFFFFFFFF },
	};
	uint16_t quad_indices[] = { 0, 1, 2, 0, 2, 3 };
	skr_mesh_create(&su_vertex_type, skr_index_fmt_u16, quad_verts, 4, quad_indices, 6, &scene->quad_mesh);
	skr_mesh_set_name(&scene->quad_mesh, "gaussian_quad");

	// Load render shader
	scene->render_shader = su_shader_load("shaders/gaussian_splat.hlsl.sks", "gaussian_splat");
	if (!skr_shader_is_valid(&scene->render_shader)) {
		su_log(su_log_warning, "gaussian_splat: Failed to load render shader");
	}

	// Create material with alpha blending (premultiplied blend, no depth write)
	skr_material_create((skr_material_info_t){
		.shader       = &scene->render_shader,
		.cull         = skr_cull_none,
		.write_mask   = skr_write_rgba,  // No depth write for proper alpha blending
		.depth_test   = skr_compare_less_or_eq,
		.blend_state  = skr_blend_premultiplied,
		.queue_offset = 100,  // Render after opaque objects
	}, &scene->render_material);

	// Load bitonic sort compute shader (legacy)
	scene->sort_shader = su_shader_load("shaders/gaussian_splat_sort.hlsl.sks", "gaussian_sort");
	if (skr_shader_is_valid(&scene->sort_shader)) {
		skr_compute_create(&scene->sort_shader, &scene->sort_compute);
		skr_compute_set_buffer(&scene->sort_compute, "splats", &scene->splat_buffer);
		skr_compute_set_buffer(&scene->sort_compute, "sort_indices", &scene->sort_index_buffer);
		skr_compute_set_buffer(&scene->sort_compute, "sort_depths", &scene->sort_depth_buffer);
	}

	// Load radix sort compute shader
	scene->radix_shader = su_shader_load("shaders/gaussian_splat_radix.hlsl.sks", "gaussian_radix");
	if (skr_shader_is_valid(&scene->radix_shader)) {
		skr_compute_create(&scene->radix_shader, &scene->radix_compute);
		skr_compute_set_buffer(&scene->radix_compute, "splats",      &scene->splat_buffer);
		skr_compute_set_buffer(&scene->radix_compute, "indices_a",   &scene->sort_index_buffer);
		skr_compute_set_buffer(&scene->radix_compute, "depths_a",    &scene->sort_depth_buffer);
		skr_compute_set_buffer(&scene->radix_compute, "indices_b",   &scene->sort_index_buffer_b);
		skr_compute_set_buffer(&scene->radix_compute, "depths_b",    &scene->sort_depth_buffer_b);
		skr_compute_set_buffer(&scene->radix_compute, "histogram",   &scene->histogram_buffer);
		skr_compute_set_buffer(&scene->radix_compute, "ranks",       &scene->ranks_buffer);
	}

	su_log(su_log_info, "gaussian_splat: Scene created with %u splats", scene->splat_count);
	su_log(su_log_info, "gaussian_splat: Camera at distance %.2f, target (%.2f, %.2f, %.2f)",
	       scene->cam_distance, scene->cam_target.x, scene->cam_target.y, scene->cam_target.z);

	return (scene_t*)scene;
}

static void _scene_gaussian_splat_destroy(scene_t* base) {
	scene_gaussian_splat_t* scene = (scene_gaussian_splat_t*)base;

	skr_buffer_destroy(&scene->splat_buffer);
	skr_buffer_destroy(&scene->sort_index_buffer);
	skr_buffer_destroy(&scene->sort_depth_buffer);
	skr_buffer_destroy(&scene->sort_index_buffer_b);
	skr_buffer_destroy(&scene->sort_depth_buffer_b);
	skr_buffer_destroy(&scene->histogram_buffer);
	skr_buffer_destroy(&scene->ranks_buffer);
	skr_mesh_destroy(&scene->quad_mesh);
	skr_material_destroy(&scene->render_material);
	skr_compute_destroy(&scene->sort_compute);
	skr_compute_destroy(&scene->radix_compute);
	skr_shader_destroy(&scene->sort_shader);
	skr_shader_destroy(&scene->radix_shader);
	skr_shader_destroy(&scene->render_shader);

	if (scene->ply_path) free(scene->ply_path);

	free(scene);
}

static void _scene_gaussian_splat_update(scene_t* base, float delta_time) {
	scene_gaussian_splat_t* scene = (scene_gaussian_splat_t*)base;
	scene->time += delta_time;

	// Camera control (same as scene_text)
	const float rotate_sensitivity = 0.003f;
	const float pan_sensitivity    = 0.002f;
	const float zoom_sensitivity   = 0.2f;
	const float velocity_damping   = 0.0001f;
	const float pitch_limit        = 1.5f;
	const float min_distance       = 0.5f;
	const float max_distance       = 100.0f;

	ImGuiIO* io = igGetIO();

	if (!io->WantCaptureMouse) {
		// Left mouse: arc rotate
		if (io->MouseDown[0]) {
			scene->cam_yaw_vel   -= io->MouseDelta.x * rotate_sensitivity;
			scene->cam_pitch_vel += io->MouseDelta.y * rotate_sensitivity;
		}

		// Right mouse: pan
		if (io->MouseDown[1]) {
			float cos_yaw = cosf(scene->cam_yaw);
			float sin_yaw = sinf(scene->cam_yaw);

			float3 right = { cos_yaw, 0.0f, -sin_yaw };

			float pan_scale = scene->cam_distance * pan_sensitivity;
			scene->cam_target_vel.x -= right.x * io->MouseDelta.x * pan_scale;
			scene->cam_target_vel.z -= right.z * io->MouseDelta.x * pan_scale;
			scene->cam_target_vel.y += io->MouseDelta.y * pan_scale;
		}

		// Scroll wheel: zoom
		if (io->MouseWheel != 0.0f) {
			scene->cam_distance_vel -= io->MouseWheel * zoom_sensitivity * scene->cam_distance * 0.1f;
		}
	}

	// Apply velocities
	scene->cam_yaw      += scene->cam_yaw_vel;
	scene->cam_pitch    += scene->cam_pitch_vel;
	scene->cam_distance += scene->cam_distance_vel;
	scene->cam_target.x += scene->cam_target_vel.x;
	scene->cam_target.y += scene->cam_target_vel.y;
	scene->cam_target.z += scene->cam_target_vel.z;

	// Clamp
	if (scene->cam_pitch >  pitch_limit) scene->cam_pitch =  pitch_limit;
	if (scene->cam_pitch < -pitch_limit) scene->cam_pitch = -pitch_limit;
	if (scene->cam_distance < min_distance) scene->cam_distance = min_distance;
	if (scene->cam_distance > max_distance) scene->cam_distance = max_distance;

	// Damping
	float damping = powf(velocity_damping, delta_time);
	scene->cam_yaw_vel      *= damping;
	scene->cam_pitch_vel    *= damping;
	scene->cam_distance_vel *= damping;
	scene->cam_target_vel.x *= damping;
	scene->cam_target_vel.y *= damping;
	scene->cam_target_vel.z *= damping;
}

// Check if two matrices are approximately equal
static bool _mat4_approx_equal(float4x4 a, float4x4 b, float epsilon) {
	for (int32_t i = 0; i < 16; i++) {
		float diff = ((float*)&a)[i] - ((float*)&b)[i];
		if (diff > epsilon || diff < -epsilon) return false;
	}
	return true;
}

// Run radix/counting sort - O(n) sort using depth quantization
// ONE pass per frame to avoid parameter buffer race conditions
// Pass sequence: 5 (init, once) -> 0 (clear) -> 1 (histogram) -> 2 (prefix) -> 3 (scatter) -> 4 (copy) -> repeat from 0
// Sort is STABLE due to: ranks buffer preserves within-bin ordering from histogram pass
// Re-sort is SKIPPED when camera stationary to prevent rank shuffling from non-deterministic atomics
static void _run_radix_sort(scene_gaussian_splat_t* scene, float4x4 view_mat) {
	if (!skr_compute_is_valid(&scene->radix_compute)) return;

	uint32_t dispatch_count = (scene->splat_count + 255) / 256;

	// Depth range for quantization - use a large range to cover the scene
	float depth_min   = -500.0f;
	float depth_range = 1000.0f;

	// Set common parameters
	skr_compute_set_param(&scene->radix_compute, "view_matrix", sksc_shader_var_float, 16, &view_mat);
	skr_compute_set_param(&scene->radix_compute, "splat_count", sksc_shader_var_uint,  1, &scene->splat_count);
	skr_compute_set_param(&scene->radix_compute, "depth_min",   sksc_shader_var_float, 1, &depth_min);
	skr_compute_set_param(&scene->radix_compute, "depth_range", sksc_shader_var_float, 1, &depth_range);

	// First time: initialize indices to identity (Pass 5)
	if (!scene->sort_depths_computed) {
		skr_compute_set_param(&scene->radix_compute, "sort_pass", sksc_shader_var_uint, 1, &(uint32_t){5});
		skr_compute_execute(&scene->radix_compute, dispatch_count, 1, 1);

		scene->sort_depths_computed = true;
		scene->radix_pass = 0;
		scene->needs_resort = true;  // Need initial sort
		su_log(su_log_info, "gaussian_splat: Radix init (pass 5)");
		return;
	}

	// Check if camera moved since last completed sort
	bool camera_moved = !_mat4_approx_equal(view_mat, scene->last_sorted_view_mat, 0.0001f);
	if (camera_moved) {
		scene->needs_resort = true;
	}

	// Skip sorting if camera stationary and we've completed at least one sort
	// This prevents rank shuffling from non-deterministic atomics
	if (!scene->needs_resort && scene->initial_sort_complete) {
		return;
	}

	// Do ONE pass per frame to avoid parameter buffer race
	uint32_t pass = scene->radix_pass;
	skr_compute_set_param(&scene->radix_compute, "sort_pass", sksc_shader_var_uint, 1, &pass);

	// Dispatch with appropriate thread count for each pass
	if (pass == 0) {
		// Clear histogram - need enough threads for 65536 bins
		skr_compute_execute(&scene->radix_compute, (65536 + 255) / 256, 1, 1);
	} else if (pass == 2) {
		// Prefix sum - single workgroup (sequential scan)
		skr_compute_execute(&scene->radix_compute, 1, 1, 1);
	} else {
		// All other passes need threads for all splats
		skr_compute_execute(&scene->radix_compute, dispatch_count, 1, 1);
	}

	// Advance to next pass (cycle: 0 -> 1 -> 2 -> 3 -> 4 -> 0)
	scene->radix_pass = (scene->radix_pass + 1) % 5;

	// Mark sort complete after pass 4 (copy back)
	if (pass == 4) {
		scene->initial_sort_complete = true;
		scene->last_sorted_view_mat = view_mat;
		scene->needs_resort = false;  // Sort complete, wait for camera movement
	}
}

// Run bitonic sort compute shader (legacy) - called from render
// Sort modes:
//   Pass 0: Initialize indices + compute depths (first time only)
//   Pass 1: Update depths only (keep sorted indices)
//   Pass 3: Bitonic sort step (for initial full sort)
static void _run_bitonic_sort(scene_gaussian_splat_t* scene, float4x4 view_mat) {
	if (!skr_compute_is_valid(&scene->sort_compute)) return;

	uint32_t dispatch_count = (scene->splat_count + 255) / 256;

	// Compute max stage needed for bitonic sort
	uint32_t n_pow2 = 1;
	while (n_pow2 < scene->splat_count) n_pow2 *= 2;
	uint32_t max_stage = 0;
	while ((1u << max_stage) < n_pow2) max_stage++;

	if (!scene->sort_depths_computed) {
		// First time: initialize indices and compute depths (Pass 0)
		skr_compute_set_param(&scene->sort_compute, "view_matrix", sksc_shader_var_float, 16, &view_mat);
		skr_compute_set_param(&scene->sort_compute, "splat_count", sksc_shader_var_uint, 1, &scene->splat_count);
		skr_compute_set_param(&scene->sort_compute, "sort_pass",   sksc_shader_var_uint, 1, &(uint32_t){0});
		skr_compute_execute(&scene->sort_compute, dispatch_count, 1, 1);
		scene->sort_depths_computed = true;
		scene->sort_stage = 1;
		scene->sort_step  = 1;
	} else if (!scene->initial_sort_complete) {
		// Initial sort: use bitonic (Pass 3)
		if (scene->sort_stage > max_stage) {
			scene->initial_sort_complete = true;
		} else {
			skr_compute_set_param(&scene->sort_compute, "splat_count", sksc_shader_var_uint, 1, &scene->splat_count);
			skr_compute_set_param(&scene->sort_compute, "sort_pass",   sksc_shader_var_uint, 1, &(uint32_t){3});
			skr_compute_set_param(&scene->sort_compute, "sort_stage",  sksc_shader_var_uint, 1, &scene->sort_stage);
			skr_compute_set_param(&scene->sort_compute, "sort_step",   sksc_shader_var_uint, 1, &scene->sort_step);
			skr_compute_execute(&scene->sort_compute, dispatch_count, 1, 1);

			if (scene->sort_step > 1) {
				scene->sort_step--;
			} else {
				scene->sort_stage++;
				scene->sort_step = scene->sort_stage;
			}
		}
	} else {
		// Refinement mode: update depths then partial bitonic
		skr_compute_set_param(&scene->sort_compute, "view_matrix", sksc_shader_var_float, 16, &view_mat);
		skr_compute_set_param(&scene->sort_compute, "splat_count", sksc_shader_var_uint, 1, &scene->splat_count);
		skr_compute_set_param(&scene->sort_compute, "sort_pass",   sksc_shader_var_uint, 1, &(uint32_t){1});
		skr_compute_execute(&scene->sort_compute, dispatch_count, 1, 1);

		const uint32_t refinement_stages = 6;
		for (uint32_t stage = 1; stage <= refinement_stages && stage <= max_stage; stage++) {
			for (uint32_t step = stage; step >= 1; step--) {
				skr_compute_set_param(&scene->sort_compute, "splat_count", sksc_shader_var_uint, 1, &scene->splat_count);
				skr_compute_set_param(&scene->sort_compute, "sort_pass",   sksc_shader_var_uint, 1, &(uint32_t){3});
				skr_compute_set_param(&scene->sort_compute, "sort_stage",  sksc_shader_var_uint, 1, &stage);
				skr_compute_set_param(&scene->sort_compute, "sort_step",   sksc_shader_var_uint, 1, &step);
				skr_compute_execute(&scene->sort_compute, dispatch_count, 1, 1);
			}
		}
	}
}

// Run sort compute shader - called from render to ensure compute and graphics are in same batch
static void _run_sort_compute(scene_gaussian_splat_t* scene) {
	if (!scene->enable_sort || scene->splat_count == 0) return;

	// Compute view matrix from camera for depth calculation
	float cos_pitch = cosf(scene->cam_pitch);
	float sin_pitch = sinf(scene->cam_pitch);
	float cos_yaw   = cosf(scene->cam_yaw);
	float sin_yaw   = sinf(scene->cam_yaw);

	float3 cam_pos = {
		scene->cam_target.x + scene->cam_distance * cos_pitch * sin_yaw,
		scene->cam_target.y + scene->cam_distance * sin_pitch,
		scene->cam_target.z + scene->cam_distance * cos_pitch * cos_yaw
	};
	float4x4 view_mat = float4x4_lookat(cam_pos, scene->cam_target, (float3){0, 1, 0});

	if (scene->use_radix_sort) {
		_run_radix_sort(scene, view_mat);
	} else {
		_run_bitonic_sort(scene, view_mat);
	}
}

static void _scene_gaussian_splat_render(scene_t* base, int32_t width, int32_t height,
                                          skr_render_list_t* ref_render_list,
                                          su_system_buffer_t* ref_system_buffer) {
	scene_gaussian_splat_t* scene = (scene_gaussian_splat_t*)base;

	if (scene->splat_count == 0) return;

	// Run compute shader for sorting (in render to ensure same command batch as draw)
	_run_sort_compute(scene);

	// Set shader parameters
	float2 screen_size = { (float)width, (float)height };
	skr_material_set_param(&scene->render_material, "splat_scale",   sksc_shader_var_float, 1, &scene->splat_scale);
	skr_material_set_param(&scene->render_material, "opacity_scale", sksc_shader_var_float, 1, &scene->opacity_scale);
	skr_material_set_param(&scene->render_material, "splat_count",   sksc_shader_var_uint,  1, &scene->splat_count);
	skr_material_set_param(&scene->render_material, "sh_degree",     sksc_shader_var_float, 1, &(float){(float)scene->sh_degree});
	skr_material_set_param(&scene->render_material, "screen_size",   sksc_shader_var_float, 2, &screen_size);
	skr_material_set_param(&scene->render_material, "max_radius",    sksc_shader_var_float, 1, &scene->max_radius);

	// Bind buffers
	skr_material_set_buffer(&scene->render_material, "splats",       &scene->splat_buffer);
	skr_material_set_buffer(&scene->render_material, "sort_indices", &scene->sort_index_buffer);

	// Render all splats as instanced quads
	skr_render_list_add(ref_render_list, &scene->quad_mesh, &scene->render_material, NULL, 0, scene->splat_count);
}

static bool _scene_gaussian_splat_get_camera(scene_t* base, scene_camera_t* out_camera) {
	scene_gaussian_splat_t* scene = (scene_gaussian_splat_t*)base;

	float cos_pitch = cosf(scene->cam_pitch);
	float sin_pitch = sinf(scene->cam_pitch);
	float cos_yaw   = cosf(scene->cam_yaw);
	float sin_yaw   = sinf(scene->cam_yaw);

	out_camera->position = (float3){
		scene->cam_target.x + scene->cam_distance * cos_pitch * sin_yaw,
		scene->cam_target.y + scene->cam_distance * sin_pitch,
		scene->cam_target.z + scene->cam_distance * cos_pitch * cos_yaw
	};
	out_camera->target = scene->cam_target;
	out_camera->up     = (float3){0.0f, 1.0f, 0.0f};

	return true;
}

static void _scene_gaussian_splat_render_ui(scene_t* base) {
	scene_gaussian_splat_t* scene = (scene_gaussian_splat_t*)base;

	igText("Gaussian Splatting");
	igSeparator();

	igText("Splats: %u", scene->splat_count);
	igSliderFloat("Splat Scale", &scene->splat_scale, 0.1f, 5.0f, "%.2f", 0);
	igSliderFloat("Opacity", &scene->opacity_scale, 0.1f, 2.0f, "%.2f", 0);
	igSliderFloat("Max Radius", &scene->max_radius, 0.0f, 1024.0f, "%.0f px", 0);
	igSliderInt("SH Degree", &scene->sh_degree, 0, 3, "%d", 0);
	igCheckbox("Enable Sorting", &scene->enable_sort);
	if (scene->enable_sort) {
		igSameLine(0, 10);
		igCheckbox("Use Radix", &scene->use_radix_sort);
	}
	if (scene->enable_sort && scene->splat_count > 0) {
		if (scene->use_radix_sort) {
			if (scene->needs_resort || !scene->initial_sort_complete) {
				const char* pass_names[] = {"Clear", "Histogram", "Prefix", "Scatter", "Copy"};
				igText("Radix pass %u: %s", scene->radix_pass, pass_names[scene->radix_pass]);
			} else {
				igText("Sorted (stable)");
			}
		} else if (!scene->initial_sort_complete) {
			// Show bitonic sort progress
			uint32_t n_pow2 = 1;
			while (n_pow2 < scene->splat_count) n_pow2 *= 2;
			uint32_t max_stage = 0;
			while ((1u << max_stage) < n_pow2) max_stage++;

			uint32_t total_steps = max_stage * (max_stage + 1) / 2;
			uint32_t completed_steps = 0;
			for (uint32_t s = 1; s < scene->sort_stage; s++) completed_steps += s;
			completed_steps += (scene->sort_stage - scene->sort_step);

			float progress = (float)completed_steps / (float)total_steps;
			igProgressBar(progress, (ImVec2){-1, 0}, NULL);
			igSameLine(0, 5);
			igText("Bitonic %u/%u", scene->sort_stage, max_stage);
		} else {
			igText("Bitonic refinement");
		}
	}

	igSeparator();
	igText("PLY: %s", scene->ply_path ? scene->ply_path : "(none)");
	if (su_file_dialog_supported()) {
		if (igButton("Load PLY...", (ImVec2){0, 0})) {
			char* path = su_file_dialog_open("Select Gaussian Splat", "PLY Files", "ply");
			if (path) {
				// Destroy old buffers
				skr_buffer_destroy(&scene->splat_buffer);
				skr_buffer_destroy(&scene->sort_index_buffer);
				skr_buffer_destroy(&scene->sort_depth_buffer);
				skr_buffer_destroy(&scene->sort_index_buffer_b);
				skr_buffer_destroy(&scene->sort_depth_buffer_b);
				skr_buffer_destroy(&scene->histogram_buffer);
				skr_buffer_destroy(&scene->ranks_buffer);
				scene->splat_count = 0;
				scene->sort_depths_computed = false;
				scene->initial_sort_complete = false;
				scene->radix_pass = 0;

				// Load new file
				if (_load_splat_ply(scene, path)) {
					if (scene->ply_path) free(scene->ply_path);
					scene->ply_path = path;

					// Update compute bindings
					if (skr_compute_is_valid(&scene->sort_compute)) {
						skr_compute_set_buffer(&scene->sort_compute, "splats", &scene->splat_buffer);
						skr_compute_set_buffer(&scene->sort_compute, "sort_indices", &scene->sort_index_buffer);
						skr_compute_set_buffer(&scene->sort_compute, "sort_depths", &scene->sort_depth_buffer);
					}
					if (skr_compute_is_valid(&scene->radix_compute)) {
						skr_compute_set_buffer(&scene->radix_compute, "splats",    &scene->splat_buffer);
						skr_compute_set_buffer(&scene->radix_compute, "indices_a", &scene->sort_index_buffer);
						skr_compute_set_buffer(&scene->radix_compute, "depths_a",  &scene->sort_depth_buffer);
						skr_compute_set_buffer(&scene->radix_compute, "indices_b", &scene->sort_index_buffer_b);
						skr_compute_set_buffer(&scene->radix_compute, "depths_b",  &scene->sort_depth_buffer_b);
						skr_compute_set_buffer(&scene->radix_compute, "histogram", &scene->histogram_buffer);
						skr_compute_set_buffer(&scene->radix_compute, "ranks",     &scene->ranks_buffer);
					}
				} else {
					free(path);
				}
			}
		}
	}

	igSeparator();
	if (igButton("Reset Camera", (ImVec2){0, 0})) {
		scene->cam_yaw          = 0.0f;
		scene->cam_pitch        = 0.0f;
		scene->cam_yaw_vel      = 0.0f;
		scene->cam_pitch_vel    = 0.0f;
		scene->cam_distance_vel = 0.0f;
		scene->cam_target_vel   = (float3){0, 0, 0};
	}

	igText("Camera Distance: %.1f", scene->cam_distance);
}

const scene_vtable_t scene_gaussian_splat_vtable = {
	.name       = "Gaussian Splat",
	.create     = _scene_gaussian_splat_create,
	.destroy    = _scene_gaussian_splat_destroy,
	.update     = _scene_gaussian_splat_update,
	.render     = _scene_gaussian_splat_render,
	.get_camera = _scene_gaussian_splat_get_camera,
	.render_ui  = _scene_gaussian_splat_render_ui,
};
