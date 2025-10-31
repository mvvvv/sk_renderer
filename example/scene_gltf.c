// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "scene.h"
#include "scene_util.h"
#include "app.h"

#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// Maximum number of meshes and textures we'll support
#define MAX_GLTF_MESHES   32
#define MAX_GLTF_TEXTURES 16

// Texture types for PBR materials
typedef enum {
	gltf_tex_type_albedo = 0,
	gltf_tex_type_metallic_roughness,
	gltf_tex_type_normal,
	gltf_tex_type_occlusion,
	gltf_tex_type_emissive,
	gltf_tex_type_count
} gltf_tex_type_;

// State for async loading
typedef enum {
	gltf_load_state_loading,
	gltf_load_state_ready,
	gltf_load_state_error
} gltf_load_state_;

// Data structure for a loaded GLTF mesh primitive
typedef struct {
	skr_vertex_pnuc_t* vertices;
	uint16_t*          indices;
	int32_t            vertex_count;
	int32_t            index_count;
	int32_t            texture_indices[gltf_tex_type_count];  // Indices per texture type, -1 if none
	HMM_Mat4           transform;                             // Node transform
	float              metallic_factor;                       // Material metallic factor
	float              roughness_factor;                      // Material roughness factor
	skr_vec4_t         base_color_factor;                     // Material base color
	skr_vec3_t         emissive_factor;                       // Material emissive factor
} gltf_mesh_data_t;

// Data structure for a loaded texture
typedef struct {
	unsigned char* pixels;
	int32_t        width;
	int32_t        height;
	bool           valid;
} gltf_texture_data_t;

// Data shared between main thread and loading thread
typedef struct {
	pthread_t              thread;
	gltf_load_state_       state;

	// Loaded data (populated by loading thread)
	gltf_mesh_data_t       meshes[MAX_GLTF_MESHES];
	int32_t                mesh_count;
	gltf_texture_data_t    textures[MAX_GLTF_TEXTURES];
	int32_t                texture_count;

	// Reference to scene (to access vertex_type, shader, white_texture)
	struct scene_gltf_t*   scene;

	// File path
	char                   filepath[256];
} gltf_load_context_t;

// GLTF scene - displays a loaded GLTF model
typedef struct scene_gltf_t {
	scene_t              base;

	skr_shader_t         shader;
	skr_material_t       materials[MAX_GLTF_MESHES];
	skr_mesh_t           meshes   [MAX_GLTF_MESHES];
	skr_tex_t            textures [MAX_GLTF_TEXTURES];
	skr_tex_t            white_texture;
	skr_tex_t            black_texture;           // Fallback for emission/occlusion
	skr_tex_t            white_metal_texture;     // Fallback for metal/rough (white = full metal/rough)
	int32_t              mesh_count;
	int32_t              texture_count;

	HMM_Mat4             transforms[MAX_GLTF_MESHES];

	gltf_load_context_t* load_ctx;

	// Placeholder sphere while loading
	skr_mesh_t           placeholder_mesh;
	skr_material_t       placeholder_material;

	// Cubemap skybox
	skr_tex_t            cubemap_texture;
	skr_tex_t            equirect_texture;          // Equirectangular source texture, these CAN'T be on the stack, due to enqueue behavior saving their pointers when setting materials!
	skr_material_t       equirect_convert_material; // Material for equirect->cubemap conversion
	skr_shader_t         equirect_to_cubemap_shader;
	skr_shader_t         skybox_shader;
	skr_shader_t         mipgen_shader;
	skr_material_t       skybox_material;
	skr_mesh_t           skybox_mesh;
	bool                 cubemap_ready;

	float                rotation;
} scene_gltf_t;

// Helper to read vertex attribute with default value
static void _read_attribute(cgltf_accessor* accessor, int32_t index, float* out_data, int32_t component_count, const float* default_value) {
	if (accessor && cgltf_accessor_read_float(accessor, index, out_data, component_count)) {
		return;
	}
	for (int32_t i = 0; i < component_count; i++) {
		out_data[i] = default_value[i];
	}
}

// Helper to calculate node transform from GLTF node
static HMM_Mat4 _calculate_node_transform(cgltf_node* node) {
	HMM_Mat4 local_transform = HMM_M4D(1.0f);

	if (node->has_matrix) {
		memcpy(&local_transform, node->matrix, sizeof(float) * 16);
		return HMM_Transpose(local_transform);  // cgltf uses column-major
	}

	// Build from TRS
	HMM_Mat4 t_mat = HMM_Translate(node->has_translation ? HMM_V3(node->translation[0], node->translation[1], node->translation[2])                    : HMM_V3(0,0,0));
	HMM_Mat4 r_mat = HMM_QToM4    (node->has_rotation    ? HMM_Q (node->rotation   [0], node->rotation   [1], node->rotation   [2], node->rotation[3]) : HMM_Q (0,0,0,1));
	HMM_Mat4 s_mat = HMM_Scale    (node->has_scale       ? HMM_V3(node->scale      [0], node->scale      [1], node->scale      [2])                    : HMM_V3(1,1,1));
	return HMM_MulM4(HMM_MulM4(t_mat, r_mat), s_mat);
}

// Helper to traverse GLTF node hierarchy and extract meshes
static void _extract_gltf_node(cgltf_data* data, cgltf_node* node, HMM_Mat4 parent_transform, gltf_load_context_t* ctx) {
	HMM_Mat4 local_transform = _calculate_node_transform(node);
	HMM_Mat4 world_transform = HMM_MulM4(parent_transform, local_transform);

	// Process mesh if present
	if (node->mesh && ctx->mesh_count < MAX_GLTF_MESHES) {
		cgltf_mesh* mesh = node->mesh;

		// For simplicity, we'll only handle the first primitive
		if (mesh->primitives_count > 0) {
			cgltf_primitive* prim = &mesh->primitives[0];

			// Only handle triangles
			if (prim->type != cgltf_primitive_type_triangles) {
				return;
			}

			gltf_mesh_data_t* mesh_data = &ctx->meshes[ctx->mesh_count];
			mesh_data->transform = world_transform;

			// Initialize texture indices to -1 (no texture)
			for (int32_t i = 0; i < gltf_tex_type_count; i++) {
				mesh_data->texture_indices[i] = -1;
			}

			// Initialize material factors to defaults
			mesh_data->metallic_factor     = 1.0f;
			mesh_data->roughness_factor    = 1.0f;
			mesh_data->base_color_factor   = (skr_vec4_t){1.0f, 1.0f, 1.0f, 1.0f};
			mesh_data->emissive_factor     = (skr_vec3_t){0.0f, 0.0f, 0.0f};

			// Get vertex count from first accessor
			cgltf_accessor* pos_accessor   = NULL;
			cgltf_accessor* norm_accessor  = NULL;
			cgltf_accessor* uv_accessor    = NULL;
			cgltf_accessor* color_accessor = NULL;

			for (size_t i = 0; i < prim->attributes_count; i++) {
				cgltf_attribute* attr = &prim->attributes[i];
				if      (attr->type == cgltf_attribute_type_position)                     { pos_accessor   = attr->data; } 
				else if (attr->type == cgltf_attribute_type_normal)                       { norm_accessor  = attr->data; } 
				else if (attr->type == cgltf_attribute_type_texcoord && attr->index == 0) { uv_accessor    = attr->data; } 
				else if (attr->type == cgltf_attribute_type_color    && attr->index == 0) { color_accessor = attr->data; }
			}

			if (!pos_accessor) {
				return;  // Need at least positions
			}

			mesh_data->vertex_count = (int32_t)pos_accessor->count;
			mesh_data->vertices = calloc(mesh_data->vertex_count, sizeof(skr_vertex_pnuc_t));

			// Extract vertices
			for (int32_t v = 0; v < mesh_data->vertex_count; v++) {
				skr_vertex_pnuc_t* vert = &mesh_data->vertices[v];
				float data[4];

				_read_attribute(pos_accessor,   v, data, 3, (float[]){0, 0, 0});
				vert->position = (skr_vec4_t){data[0], data[1], data[2], 1.0f};

				_read_attribute(norm_accessor,  v, data, 3, (float[]){0, 1, 0});
				vert->normal = (skr_vec3_t){data[0], data[1], data[2]};

				_read_attribute(uv_accessor,    v, data, 2, (float[]){0, 0});
				vert->uv = (skr_vec2_t){data[0], data[1]};

				_read_attribute(color_accessor, v, data, 4, (float[]){1, 1, 1, 1});
				vert->color = (skr_vec4_t){data[0], data[1], data[2], data[3]};
			}

			// Extract indices
			if (prim->indices) {
				mesh_data->index_count = (int32_t)prim->indices->count;
				mesh_data->indices = malloc(mesh_data->index_count * sizeof(uint16_t));

				for (int32_t i = 0; i < mesh_data->index_count; i++) {
					mesh_data->indices[i] = (uint16_t)cgltf_accessor_read_index(prim->indices, i);
				}
			}

			// Extract material properties and textures
			if (prim->material) {
				cgltf_material* mat = prim->material;
				cgltf_pbr_metallic_roughness* pbr = &mat->pbr_metallic_roughness;

				// Extract material factors
				mesh_data->metallic_factor = pbr->metallic_factor;
				mesh_data->roughness_factor = pbr->roughness_factor;
				mesh_data->base_color_factor = (skr_vec4_t){
					pbr->base_color_factor[0],
					pbr->base_color_factor[1],
					pbr->base_color_factor[2],
					pbr->base_color_factor[3]
				};
				mesh_data->emissive_factor = (skr_vec3_t){
					mat->emissive_factor[0],
					mat->emissive_factor[1],
					mat->emissive_factor[2]
				};

				// Albedo/base color texture
				if (pbr->base_color_texture.texture && pbr->base_color_texture.texture->image) {
					cgltf_image* img = pbr->base_color_texture.texture->image;
					for (size_t t = 0; t < data->images_count; t++) {
						if ((void*)img == (void*)&data->images[t]) {
							mesh_data->texture_indices[gltf_tex_type_albedo] = (int32_t)t;
							break;
						}
					}
				}

				// Metallic-roughness texture
				if (pbr->metallic_roughness_texture.texture && pbr->metallic_roughness_texture.texture->image) {
					cgltf_image* img = pbr->metallic_roughness_texture.texture->image;
					for (size_t t = 0; t < data->images_count; t++) {
						if ((void*)img == (void*)&data->images[t]) {
							mesh_data->texture_indices[gltf_tex_type_metallic_roughness] = (int32_t)t;
							break;
						}
					}
				}

				// Normal texture
				if (mat->normal_texture.texture && mat->normal_texture.texture->image) {
					cgltf_image* img = mat->normal_texture.texture->image;
					for (size_t t = 0; t < data->images_count; t++) {
						if ((void*)img == (void*)&data->images[t]) {
							mesh_data->texture_indices[gltf_tex_type_normal] = (int32_t)t;
							break;
						}
					}
				}

				// Occlusion texture
				if (mat->occlusion_texture.texture && mat->occlusion_texture.texture->image) {
					cgltf_image* img = mat->occlusion_texture.texture->image;
					for (size_t t = 0; t < data->images_count; t++) {
						if ((void*)img == (void*)&data->images[t]) {
							mesh_data->texture_indices[gltf_tex_type_occlusion] = (int32_t)t;
							break;
						}
					}
				}

				// Emissive texture
				if (mat->emissive_texture.texture && mat->emissive_texture.texture->image) {
					cgltf_image* img = mat->emissive_texture.texture->image;
					for (size_t t = 0; t < data->images_count; t++) {
						if ((void*)img == (void*)&data->images[t]) {
							mesh_data->texture_indices[gltf_tex_type_emissive] = (int32_t)t;
							break;
						}
					}
				}
			}

			ctx->mesh_count++;
		}
	}

	// Recurse to children
	for (size_t i = 0; i < node->children_count; i++) {
		_extract_gltf_node(data, node->children[i], world_transform, ctx);
	}
}

// Helper to load texture from buffer view
static bool _load_texture_from_buffer_view(cgltf_image* img, gltf_texture_data_t* tex_data) {
	cgltf_buffer_view* view     = img->buffer_view;
	unsigned char*     img_data = (unsigned char*)view->buffer->data + view->offset;
	tex_data->pixels = skr_image_load_from_memory(img_data, view->size, &tex_data->width, &tex_data->height, NULL, 4);
	return tex_data->pixels != NULL;
}

// Helper to load texture from data URI
static bool _load_texture_from_data_uri(cgltf_image* img, cgltf_options* options, gltf_texture_data_t* tex_data) {
	char* comma = strchr(img->uri, ',');
	if (!comma || comma - img->uri < 7 || strncmp(comma - 7, ";base64", 7) != 0) {
		return false;
	}

	char* base64_start = comma + 1;
	size_t base64_len = strlen(base64_start);
	size_t base64_size = 3 * (base64_len / 4);
	if (base64_len >= 1 && base64_start[base64_len - 1] == '=') base64_size -= 1;
	if (base64_len >= 2 && base64_start[base64_len - 2] == '=') base64_size -= 1;

	void* buffer = NULL;
	if (cgltf_load_buffer_base64(options, base64_size, base64_start, &buffer) != cgltf_result_success || !buffer) {
		return false;
	}

	tex_data->pixels = skr_image_load_from_memory((unsigned char*)buffer, base64_size, &tex_data->width, &tex_data->height, NULL, 4);
	free(buffer);
	return tex_data->pixels != NULL;
}

// Helper to load texture from external file
static bool _load_texture_from_file(const char* base_path, cgltf_image* img, gltf_texture_data_t* tex_data) {
	char texture_path[512];
	if (base_path[0] != '\0') {
		snprintf(texture_path, sizeof(texture_path), "%s%s", base_path, img->uri);
	} else {
		snprintf(texture_path, sizeof(texture_path), "%s", img->uri);
	}

	tex_data->pixels = skr_image_load(texture_path, &tex_data->width, &tex_data->height, NULL, 4);
	return tex_data->pixels != NULL;
}

// Helper to create GPU texture and bind to materials
static void _create_gpu_texture_and_bind(scene_gltf_t* scene, gltf_load_context_t* ctx, int32_t tex_index, gltf_texture_data_t* tex_data, gltf_tex_type_ tex_type) {
	if (!tex_data->valid || !tex_data->pixels) return;

	skr_tex_create(
		skr_tex_fmt_rgba32,
		skr_tex_flags_readable | skr_tex_flags_gen_mips,
		skr_sampler_linear_wrap,
		(skr_vec3i_t){tex_data->width, tex_data->height, 1},
		1, 0, tex_data->pixels, &scene->textures[tex_index]
	);

	char tex_name[64];
	const char* tex_type_names[] = {"albedo", "metal_rough", "normal", "occlusion", "emissive"};
	snprintf(tex_name, sizeof(tex_name), "gltf_%s_%d", tex_type_names[tex_type], tex_index);
	skr_tex_set_name(&scene->textures[tex_index], tex_name);

	skr_tex_generate_mips(&scene->textures[tex_index], NULL);
	scene->texture_count++;

	// Get bind slot name for this texture type
	const char* bind_names[] = {
		"albedo_tex",   // gltf_tex_type_albedo
		"metal_tex",    // gltf_tex_type_metallic_roughness
		NULL,           // gltf_tex_type_normal (not used yet)
		"occlusion_tex",// gltf_tex_type_occlusion
		"emission_tex"  // gltf_tex_type_emissive
	};

	const char* bind_name = bind_names[tex_type];
	if (!bind_name) return;  // Skip textures we don't bind yet

	// Bind to all materials that use this texture
	for (int32_t m = 0; m < ctx->mesh_count; m++) {
		if (ctx->meshes[m].texture_indices[tex_type] == tex_index) {
			// Get the bind slot from the shader
			skr_material_set_tex(&scene->materials[m], bind_name, &scene->textures[tex_index]);
		}
	}
}

// Thread function to load GLTF file
static void* _load_gltf_thread(void* arg) {
	gltf_load_context_t* ctx = (gltf_load_context_t*)arg;

	// Initialize this thread for sk_renderer
	skr_thread_init();

	skr_log(skr_log_info, "GLTF: Loading started");

	// Extract directory path for loading external resources
	char base_path[256] = "";
	const char* last_slash = strrchr(ctx->filepath, '/');
	if (last_slash) {
		size_t dir_len = last_slash - ctx->filepath + 1;
		if (dir_len < sizeof(base_path)) {
			strncpy(base_path, ctx->filepath, dir_len);
			base_path[dir_len] = '\0';
		}
	}

	// Load file
	void*  file_data = NULL;
	size_t file_size = 0;

	if (!skr_file_read(ctx->filepath, &file_data, &file_size)) {
		skr_log(skr_log_critical, "GLTF: Failed to read file");
		ctx->state = gltf_load_state_error;
		skr_thread_shutdown();
		return NULL;
	}

	// Parse GLTF with file reader configured
	cgltf_options options = {0};
	options.file.read = NULL;  // Use default file reading
	options.file.release = NULL;
	cgltf_data*   data    = NULL;
	cgltf_result  result  = cgltf_parse(&options, file_data, file_size, &data);

	if (result != cgltf_result_success) {
		skr_log(skr_log_critical, "GLTF: Failed to parse");
		free(file_data);
		ctx->state = gltf_load_state_error;
		skr_thread_shutdown();
		return NULL;
	}

	// Load buffers
	result = cgltf_load_buffers(&options, data, ctx->filepath);
	if (result != cgltf_result_success) {
		skr_log(skr_log_critical, "GLTF: Failed to load buffers");
		cgltf_free(data);
		free(file_data);
		ctx->state = gltf_load_state_error;
		skr_thread_shutdown();
		return NULL;
	}

	// Extract meshes from scene and create GPU resources
	scene_gltf_t* scene = ctx->scene;
	if (data->scene && data->scene->nodes_count > 0) {
		for (size_t i = 0; i < data->scene->nodes_count; i++) {
			_extract_gltf_node(data, data->scene->nodes[i], HMM_M4D(1.0f), ctx);
		}

		// Create GPU meshes with default materials
		for (int32_t i = 0; i < ctx->mesh_count; i++) {
			gltf_mesh_data_t* mesh_data = &ctx->meshes[i];

			skr_mesh_create(
				&skr_vertex_type_pnuc,
				skr_index_fmt_u16,
				mesh_data->vertices,
				mesh_data->vertex_count,
				mesh_data->indices,
				mesh_data->index_count, &scene->meshes[i]
			);

			char mesh_name[64];
			snprintf(mesh_name, sizeof(mesh_name), "gltf_mesh_%d", i);
			skr_mesh_set_name(&scene->meshes[i], mesh_name);

			if (skr_shader_is_valid(&scene->shader)) {
				skr_material_create((skr_material_info_t){
					.shader     = &scene->shader,
					.cull       = skr_cull_back,
					.write_mask = skr_write_default,
					.depth_test = skr_compare_less,
				}, &scene->materials[i]);

				// Set default fallback textures for all PBR slots
				skr_material_set_tex  (&scene->materials[i], "albedo_tex",      &scene->white_texture);
				skr_material_set_tex  (&scene->materials[i], "emission_tex",    &scene->black_texture);
				skr_material_set_tex  (&scene->materials[i], "metal_tex",       &scene->white_metal_texture);
				skr_material_set_tex  (&scene->materials[i], "occlusion_tex",   &scene->white_texture);
				skr_material_set_color(&scene->materials[i], "color",           (skr_vec4_t){mesh_data->base_color_factor.x, mesh_data->base_color_factor.y, mesh_data->base_color_factor.z, mesh_data->base_color_factor.w});
				skr_material_set_color(&scene->materials[i], "emission_factor", (skr_vec4_t){mesh_data->emissive_factor.x, mesh_data->emissive_factor.y, mesh_data->emissive_factor.z, 1.0f});
				skr_material_set_vec4 (&scene->materials[i], "tex_trans",       (skr_vec4_t){0.0f, 0.0f, 1.0f, 1.0f});
				skr_material_set_float(&scene->materials[i], "metallic",        mesh_data->metallic_factor);
				skr_material_set_float(&scene->materials[i], "roughness",       mesh_data->roughness_factor);
			}

			scene->transforms[i] = mesh_data->transform;
			scene->mesh_count++;
		}

		// Meshes ready - can start rendering with default materials
		ctx->state = gltf_load_state_ready;
	}

	// Load textures that are referenced by meshes
	for (int32_t m = 0; m < ctx->mesh_count; m++) {
		// Iterate over all texture types for this mesh
		for (int32_t tex_type = 0; tex_type < gltf_tex_type_count; tex_type++) {
			int32_t tex_idx = ctx->meshes[m].texture_indices[tex_type];
			if (tex_idx < 0 || tex_idx >= (int32_t)data->images_count) continue;

			gltf_texture_data_t* tex_data = &ctx->textures[tex_idx];
			if (tex_data->valid) continue;  // Already loaded

			cgltf_image* img = &data->images[tex_idx];

			// Try different loading methods
			if (img->buffer_view) {
				tex_data->valid = _load_texture_from_buffer_view(img, tex_data);
			} else if (img->uri && strncmp(img->uri, "data:", 5) == 0) {
				tex_data->valid = _load_texture_from_data_uri(img, &options, tex_data);
			} else if (img->uri) {
				tex_data->valid = _load_texture_from_file(base_path, img, tex_data);
			}

			// Create GPU texture and bind to materials
			_create_gpu_texture_and_bind(scene, ctx, tex_idx, tex_data, (gltf_tex_type_)tex_type);

			if (tex_idx >= ctx->texture_count) {
				ctx->texture_count = tex_idx + 1;
			}
		}
	}

	cgltf_free(data);
	free(file_data);

	skr_logf(skr_log_info, "GLTF: Ready (%d meshes, %d textures)", ctx->mesh_count, scene->texture_count);
	ctx->state = gltf_load_state_ready;
	skr_thread_shutdown();
	return NULL;
}

static scene_t* _scene_gltf_create() {
	scene_gltf_t* scene = calloc(1, sizeof(scene_gltf_t));
	if (!scene) return NULL;

	scene->base.size = sizeof(scene_gltf_t);
	scene->rotation  = 0.0f;

	// Create fallback textures
	scene->white_texture       = skr_tex_create_solid_color(0xFFFFFFFF);
	scene->black_texture       = skr_tex_create_solid_color(0xFF000000);
	scene->white_metal_texture = skr_tex_create_solid_color(0xFFFFFFFF);
	skr_tex_set_name(&scene->white_texture,       "gltf_white_fallback");
	skr_tex_set_name(&scene->black_texture,       "gltf_black_fallback");
	skr_tex_set_name(&scene->white_metal_texture, "gltf_metal_fallback");

	// Create placeholder sphere
	skr_vec4_t gray = {0.5f, 0.5f, 0.5f, 1.0f};
	scene->placeholder_mesh = skr_mesh_create_sphere(16, 12, 1.0f, gray);
	skr_mesh_set_name(&scene->placeholder_mesh, "gltf_placeholder_sphere");

	// Load PBR shader
	scene->shader = skr_shader_load("shaders/pbr.hlsl.sks", "pbr_shader");

	// Create placeholder material
	skr_material_create((skr_material_info_t){
		.shader     = &scene->shader,
		.cull       = skr_cull_back,
		.write_mask = skr_write_default,
		.depth_test = skr_compare_less,
	}, &scene->placeholder_material);

	// Set default textures
	skr_material_set_tex  (&scene->placeholder_material, "albedo_tex",      &scene->white_texture);
	skr_material_set_tex  (&scene->placeholder_material, "emission_tex",    &scene->black_texture);
	skr_material_set_tex  (&scene->placeholder_material, "metal_tex",       &scene->white_texture);
	skr_material_set_tex  (&scene->placeholder_material, "occlusion_tex",   &scene->white_texture);
	skr_material_set_color(&scene->placeholder_material, "color",           (skr_vec4_t){0.5f, 0.5f, 0.5f, 1.0f});
	skr_material_set_color(&scene->placeholder_material, "emission_factor", (skr_vec4_t){0.0f, 0.0f, 0.0f, 1.0f});
	skr_material_set_vec4 (&scene->placeholder_material, "tex_trans",       (skr_vec4_t){0.0f, 0.0f, 1.0f, 1.0f});
	skr_material_set_float(&scene->placeholder_material, "metallic",        0);
	skr_material_set_float(&scene->placeholder_material, "roughness",       0.8f);

	// Start loading GLTF in background thread
	scene->load_ctx = calloc(1, sizeof(gltf_load_context_t));
	scene->load_ctx->state = gltf_load_state_loading;
	scene->load_ctx->scene = scene;
	snprintf(scene->load_ctx->filepath, sizeof(scene->load_ctx->filepath), "DamagedHelmet.glb");

	pthread_create(&scene->load_ctx->thread, NULL, _load_gltf_thread, scene->load_ctx);

	// Load equirectangular image and convert to cubemap
	scene->cubemap_ready = false;
	{
		// Load equirectangular HDR or LDR image
		int32_t        equirect_width  = 0;
		int32_t        equirect_height = 0;
		unsigned char* equirect_data   = skr_image_load("cubemap.jpg", &equirect_width, &equirect_height, NULL, 4);

		if (equirect_data && equirect_width > 0 && equirect_height > 0) {
			// Create equirectangular texture (stored in scene struct to avoid stack variable)
			skr_tex_create(
				skr_tex_fmt_rgba32,
				skr_tex_flags_readable,
				skr_sampler_linear_wrap,
				(skr_vec3i_t){equirect_width, equirect_height, 1},
				1, 0, equirect_data, &scene->equirect_texture
			);
			skr_tex_set_name(&scene->equirect_texture, "equirect_source");
			skr_image_free(equirect_data);

			// Create empty cubemap texture to render into
			const int32_t cube_size = equirect_height/2;
			skr_tex_create(
				skr_tex_fmt_rgba32,
				skr_tex_flags_readable | skr_tex_flags_writeable | skr_tex_flags_cubemap | skr_tex_flags_gen_mips,
				skr_sampler_linear_clamp,
				(skr_vec3i_t){cube_size, cube_size, 6},
				1, 0, NULL, &scene->cubemap_texture
			);
			skr_tex_set_name(&scene->cubemap_texture, "environment_cubemap");

			// Load equirect to cubemap shader
			scene->equirect_to_cubemap_shader = skr_shader_load("shaders/equirect_to_cubemap.hlsl.sks", "equirect_to_cubemap");

			// Create material for conversion (stored in scene struct to avoid stack variable)
			skr_material_create((skr_material_info_t){
				.shader     = &scene->equirect_to_cubemap_shader,
				.write_mask = skr_write_rgba,
				.cull       = skr_cull_none,
			}, &scene->equirect_convert_material);
			skr_material_set_tex(&scene->equirect_convert_material, "equirect_tex", &scene->equirect_texture);

			// Convert equirectangular to cubemap using blit
			skr_renderer_blit(&scene->equirect_convert_material, &scene->cubemap_texture, (skr_recti_t){0, 0, cube_size, cube_size});

			// Wait for conversion to complete
			vkDeviceWaitIdle(skr_get_vk_device());

			skr_material_destroy(&scene->equirect_convert_material);

			// Clean up equirectangular texture
			skr_tex_destroy(&scene->equirect_texture);

			// Load cubemap mipgen shader for high-quality IBL filtering
			scene->mipgen_shader = skr_shader_load("shaders/cubemap_mipgen.hlsl.sks", "cubemap_mipgen");

			// Generate mips for the cubemap using custom shader
			skr_tex_generate_mips(&scene->cubemap_texture, &scene->mipgen_shader);

			// Load skybox shader
			scene->skybox_shader = skr_shader_load("shaders/cubemap_skybox.hlsl.sks", "skybox_shader");

			// Create skybox material
			skr_material_create((skr_material_info_t){
				.shader       = &scene->skybox_shader,
				.write_mask   = skr_write_rgba,
				.depth_test   = skr_compare_less_or_eq,
				.cull         = skr_cull_none,
				.queue_offset = 100,
			}, &scene->skybox_material);
			skr_material_set_tex(&scene->skybox_material, "cubemap", &scene->cubemap_texture);

			scene->skybox_mesh = skr_mesh_create_fullscreen_quad();
			skr_mesh_set_name(&scene->skybox_mesh, "skybox_fullscreen_quad");

			scene->cubemap_ready = true;
		}
	}

	return (scene_t*)scene;
}

static void _scene_gltf_destroy(scene_t* base) {
	scene_gltf_t* scene = (scene_gltf_t*)base;

	// Wait for loading thread to finish
	if (scene->load_ctx) {
		pthread_join(scene->load_ctx->thread, NULL);

		// Free loaded data
		for (int32_t i = 0; i < scene->load_ctx->mesh_count; i++) {
			free(scene->load_ctx->meshes[i].vertices);
			free(scene->load_ctx->meshes[i].indices);
		}
		for (int32_t i = 0; i < scene->load_ctx->texture_count; i++) {
			if (scene->load_ctx->textures[i].pixels) {
				skr_image_free(scene->load_ctx->textures[i].pixels);
			}
		}

		free(scene->load_ctx);
	}

	// Destroy placeholder
	skr_mesh_destroy    (&scene->placeholder_mesh);
	skr_material_destroy(&scene->placeholder_material);

	// Destroy renderer resources
	for (int32_t i = 0; i < scene->mesh_count; i++) {
		skr_mesh_destroy    (&scene->meshes   [i]);
		skr_material_destroy(&scene->materials[i]);
	}
	for (int32_t i = 0; i < MAX_GLTF_TEXTURES; i++) {
		skr_tex_destroy(&scene->textures[i]);
	}
	skr_tex_destroy   (&scene->white_texture);
	skr_tex_destroy   (&scene->black_texture);
	skr_tex_destroy   (&scene->white_metal_texture);
	skr_shader_destroy(&scene->shader);

	// Destroy cubemap resources
	if (scene->cubemap_ready) {
		skr_mesh_destroy    (&scene->skybox_mesh);
		skr_material_destroy(&scene->skybox_material);
		skr_shader_destroy  (&scene->skybox_shader);
		skr_shader_destroy  (&scene->mipgen_shader);
		skr_shader_destroy  (&scene->equirect_to_cubemap_shader);
		skr_tex_destroy     (&scene->cubemap_texture);
	}

	free(scene);
}

static void _scene_gltf_update(scene_t* base, float delta_time) {
	scene_gltf_t* scene = (scene_gltf_t*)base;
	scene->rotation += delta_time * 0.5f;
}

static void _scene_gltf_render(scene_t* base, int32_t width, int32_t height, HMM_Mat4 viewproj, skr_render_list_t* ref_render_list, app_system_buffer_t* ref_system_buffer) {
	scene_gltf_t*    scene = (scene_gltf_t*)base;
	gltf_load_state_ state = scene->load_ctx ? scene->load_ctx->state : gltf_load_state_loading;

	// Set up environment cubemap info in system buffer for PBR if available
	if (scene->cubemap_ready && ref_system_buffer) {
		// Set cubemap info in system buffer
		ref_system_buffer->cubemap_info[0] = (float)scene->cubemap_texture.size.x;
		ref_system_buffer->cubemap_info[1] = (float)scene->cubemap_texture.size.y;
		ref_system_buffer->cubemap_info[2] = (float)scene->cubemap_texture.mip_levels;
		ref_system_buffer->cubemap_info[3] = 0.0f;  // Unused
		ref_system_buffer->time = scene->rotation;  // Use rotation as time for now

		// Bind environment cubemap to all PBR materials
		skr_material_set_tex(&scene->placeholder_material, "environment_map", &scene->cubemap_texture);
		for (int32_t i = 0; i < scene->mesh_count; i++) {
			skr_material_set_tex(&scene->materials[i], "environment_map", &scene->cubemap_texture);
		}
	}

	// Render skybox if ready
	if (scene->cubemap_ready) {
		skr_render_list_add(ref_render_list, &scene->skybox_mesh, &scene->skybox_material, NULL, 0, 1);
	} else {return;}

	if (state != gltf_load_state_ready) {
		// Show placeholder while loading or on error
		HMM_Mat4 world = skr_matrix_trs(
			HMM_V3(0.0f, 0.0f, 0.0f),
			HMM_V3(0.0f, scene->rotation * 2.0f, 0.0f),
			HMM_V3(1.0f, 1.0f, 1.0f) );
		skr_render_list_add(ref_render_list, &scene->placeholder_mesh, &scene->placeholder_material, &world, sizeof(HMM_Mat4), 1);
		return;
	}

	// Render loaded model
	for (int32_t i = 0; i < scene->mesh_count; i++) {
		HMM_Mat4 rotation = HMM_Rotate_RH(scene->rotation, HMM_V3(0.0f, 1.0f, 0.0f));
		//HMM_Mat4 scale    = HMM_Scale(HMM_V3(80.0f, 80.0f, 80.0f));
		//HMM_Mat4 tr       = HMM_Translate(HMM_V3(0, -3, 0));
		HMM_Mat4 scale    = HMM_Scale(HMM_V3(1.0f, 1.0f, 1.0f));
		HMM_Mat4 tr       = HMM_Translate(HMM_V3(0, 0, 0));
		HMM_Mat4 world    = HMM_Transpose(HMM_MulM4(tr, HMM_MulM4(scale, HMM_MulM4(rotation, scene->transforms[i]))));
		skr_render_list_add(ref_render_list, &scene->meshes[i], &scene->materials[i], &world, sizeof(HMM_Mat4), 1);
	}
}

static bool _scene_gltf_get_camera(scene_t* base, scene_camera_t* out_camera) {
	scene_gltf_t* scene = (scene_gltf_t*)base;

	// Orbit camera
	float radius = 5.0f;
	float height = 2.0f;
	float angle = scene->rotation * 0.3f;

	out_camera->position = HMM_V3(cosf(angle) * radius, height, sinf(angle) * radius);
	out_camera->target   = HMM_V3(0.0f, 0.0f, 0.0f);
	out_camera->up       = HMM_V3(0.0f, 1.0f, 0.0f);

	return true;
}

const scene_vtable_t scene_gltf_vtable = {
	.name       = "GLTF Model",
	.create     = _scene_gltf_create,
	.destroy    = _scene_gltf_destroy,
	.update     = _scene_gltf_update,
	.render     = _scene_gltf_render,
	.get_camera = _scene_gltf_get_camera,
};
