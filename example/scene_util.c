// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "scene_util.h"
#include "app.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <threads.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <stdio.h>

#include <SDL.h>

///////////////////////////////////////////////////////////////////////////////
// Common Texture Samplers
///////////////////////////////////////////////////////////////////////////////

const skr_tex_sampler_t su_sampler_linear_clamp = {
	.sample  = skr_tex_sample_linear,
	.address = skr_tex_address_clamp
};

const skr_tex_sampler_t su_sampler_linear_wrap = {
	.sample  = skr_tex_sample_linear,
	.address = skr_tex_address_wrap
};

const skr_tex_sampler_t su_sampler_point_clamp = {
	.sample  = skr_tex_sample_point,
	.address = skr_tex_address_clamp
};

///////////////////////////////////////////////////////////////////////////////
// Standard Vertex Types
///////////////////////////////////////////////////////////////////////////////

skr_vert_type_t su_vertex_type_pnuc = {0};

static void _su_vertex_types_init(void) {
	skr_vert_type_create(
		(skr_vert_component_t[]){
			{ .format = skr_vertex_fmt_f32,            .count = 3, .semantic = skr_semantic_position, .semantic_slot = 0 },
			{ .format = skr_vertex_fmt_f32,            .count = 3, .semantic = skr_semantic_normal,   .semantic_slot = 0 },
			{ .format = skr_vertex_fmt_f32,            .count = 2, .semantic = skr_semantic_texcoord, .semantic_slot = 0 },
			{ .format = skr_vertex_fmt_ui8_normalized, .count = 4, .semantic = skr_semantic_color,    .semantic_slot = 0 }}, 4, &su_vertex_type_pnuc);
}

///////////////////////////////////////////////////////////////////////////////
// Mesh Generation
///////////////////////////////////////////////////////////////////////////////

// Helper function to convert vec4 color to packed uint32 (RGBA)
static inline uint32_t _color_vec4_to_u32(skr_vec4_t color) {
	uint8_t r = (uint8_t)(color.x * 255.0f);
	uint8_t g = (uint8_t)(color.y * 255.0f);
	uint8_t b = (uint8_t)(color.z * 255.0f);
	uint8_t a = (uint8_t)(color.w * 255.0f);
	return (a << 24) | (b << 16) | (g << 8) | r;
}

skr_mesh_t su_mesh_create_sphere(int32_t segments, int32_t rings, float radius, skr_vec4_t color) {
	const int vert_count = (rings + 1) * (segments + 1);
	const int idx_count  = rings * segments * 6;

	su_vertex_pnuc_t* verts = malloc(sizeof(su_vertex_pnuc_t) * vert_count);
	uint16_t*          inds  = malloc(sizeof(uint16_t) * idx_count);
	uint32_t           color_u32 = _color_vec4_to_u32(color);

	// Generate vertices
	int v_idx = 0;
	for (int r = 0; r <= rings; r++) {
		float v = (float)r / rings;
		float phi = v * 3.14159265359f;
		for (int s = 0; s <= segments; s++) {
			float u = (float)s / segments;
			float theta = u * 3.14159265359f * 2.0f;

			float x = sinf(phi) * cosf(theta);
			float y = cosf(phi);
			float z = sinf(phi) * sinf(theta);

			verts[v_idx++] = (su_vertex_pnuc_t){
				.position = {x * radius, y * radius, z * radius},
				.normal   = {x, y, z},
				.uv       = {u, v},
				.color    = color_u32
			};
		}
	}

	// Generate indices
	int i_idx = 0;
	for (int r = 0; r < rings; r++) {
		for (int s = 0; s < segments; s++) {
			int current = r * (segments + 1) + s;
			int next    = current + segments + 1;

			inds[i_idx++] = current + 1;
			inds[i_idx++] = next;
			inds[i_idx++] = current;

			inds[i_idx++] = next + 1;
			inds[i_idx++] = next;
			inds[i_idx++] = current + 1;
		}
	}

	skr_mesh_t mesh;
	skr_mesh_create(&su_vertex_type_pnuc, skr_index_fmt_u16, verts, vert_count, inds, idx_count, &mesh);
	free(verts);
	free(inds);

	return mesh;
}

skr_mesh_t su_mesh_create_cube(float size, const skr_vec4_t* opt_face_colors) {
	float half = size * 0.5f;

	// Default to white if no colors provided
	skr_vec4_t white = {1.0f, 1.0f, 1.0f, 1.0f};
	skr_vec4_t colors[6];
	for (int i = 0; i < 6; i++) {
		colors[i] = opt_face_colors ? opt_face_colors[i] : white;
	}

	uint32_t colors_u32[6];
	for (int i = 0; i < 6; i++) {
		colors_u32[i] = _color_vec4_to_u32(colors[i]);
	}

	su_vertex_pnuc_t verts[24] = {
		// Front face (Z+)
		{ .position = {-half, -half,  half}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {0.0f, 0.0f}, .color = colors_u32[0] },
		{ .position = { half, -half,  half}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {1.0f, 0.0f}, .color = colors_u32[0] },
		{ .position = { half,  half,  half}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {1.0f, 1.0f}, .color = colors_u32[0] },
		{ .position = {-half,  half,  half}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {0.0f, 1.0f}, .color = colors_u32[0] },
		// Back face (Z-)
		{ .position = { half, -half, -half}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {0.0f, 0.0f}, .color = colors_u32[1] },
		{ .position = {-half, -half, -half}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {1.0f, 0.0f}, .color = colors_u32[1] },
		{ .position = {-half,  half, -half}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {1.0f, 1.0f}, .color = colors_u32[1] },
		{ .position = { half,  half, -half}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {0.0f, 1.0f}, .color = colors_u32[1] },
		// Top face (Y+)
		{ .position = {-half,  half,  half}, .normal = { 0.0f,  1.0f,  0.0f}, .uv = {0.0f, 0.0f}, .color = colors_u32[2] },
		{ .position = { half,  half,  half}, .normal = { 0.0f,  1.0f,  0.0f}, .uv = {1.0f, 0.0f}, .color = colors_u32[2] },
		{ .position = { half,  half, -half}, .normal = { 0.0f,  1.0f,  0.0f}, .uv = {1.0f, 1.0f}, .color = colors_u32[2] },
		{ .position = {-half,  half, -half}, .normal = { 0.0f,  1.0f,  0.0f}, .uv = {0.0f, 1.0f}, .color = colors_u32[2] },
		// Bottom face (Y-)
		{ .position = {-half, -half, -half}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {0.0f, 0.0f}, .color = colors_u32[3] },
		{ .position = { half, -half, -half}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {1.0f, 0.0f}, .color = colors_u32[3] },
		{ .position = { half, -half,  half}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {1.0f, 1.0f}, .color = colors_u32[3] },
		{ .position = {-half, -half,  half}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {0.0f, 1.0f}, .color = colors_u32[3] },
		// Right face (X+)
		{ .position = { half, -half,  half}, .normal = { 1.0f,  0.0f,  0.0f}, .uv = {0.0f, 0.0f}, .color = colors_u32[4] },
		{ .position = { half, -half, -half}, .normal = { 1.0f,  0.0f,  0.0f}, .uv = {1.0f, 0.0f}, .color = colors_u32[4] },
		{ .position = { half,  half, -half}, .normal = { 1.0f,  0.0f,  0.0f}, .uv = {1.0f, 1.0f}, .color = colors_u32[4] },
		{ .position = { half,  half,  half}, .normal = { 1.0f,  0.0f,  0.0f}, .uv = {0.0f, 1.0f}, .color = colors_u32[4] },
		// Left face (X-)
		{ .position = {-half, -half, -half}, .normal = {-1.0f,  0.0f,  0.0f}, .uv = {0.0f, 0.0f}, .color = colors_u32[5] },
		{ .position = {-half, -half,  half}, .normal = {-1.0f,  0.0f,  0.0f}, .uv = {1.0f, 0.0f}, .color = colors_u32[5] },
		{ .position = {-half,  half,  half}, .normal = {-1.0f,  0.0f,  0.0f}, .uv = {1.0f, 1.0f}, .color = colors_u32[5] },
		{ .position = {-half,  half, -half}, .normal = {-1.0f,  0.0f,  0.0f}, .uv = {0.0f, 1.0f}, .color = colors_u32[5] },
	};

	uint16_t indices[] = {
		0, 1, 2,  2, 3, 0,
		4, 5, 6,  6, 7, 4,
		8, 9, 10, 10, 11, 8,
		12, 13, 14, 14, 15, 12,
		16, 17, 18, 18, 19, 16,
		20, 21, 22, 22, 23, 20,
	};

	skr_mesh_t mesh;
	skr_mesh_create(&su_vertex_type_pnuc, skr_index_fmt_u16, verts, 24, indices, 36, &mesh);
	return mesh;
}

skr_mesh_t su_mesh_create_pyramid(float base_size, float height, skr_vec4_t color) {
	float half = base_size * 0.5f;
	float apex_y = height * 0.5f;
	float base_y = -height * 0.5f;

	// Calculate normals for side faces
	float slant_len = sqrtf(half * half + height * height);
	float normal_y = half / slant_len;
	float normal_xz = height / slant_len;

	uint32_t color_u32 = _color_vec4_to_u32(color);

	su_vertex_pnuc_t verts[17] = {
		// Base (4 vertices)
		{ .position = {-half, base_y,  half}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {0.0f, 0.0f}, .color = color_u32 },
		{ .position = { half, base_y,  half}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {1.0f, 0.0f}, .color = color_u32 },
		{ .position = { half, base_y, -half}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {1.0f, 1.0f}, .color = color_u32 },
		{ .position = {-half, base_y, -half}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {0.0f, 1.0f}, .color = color_u32 },
		// Apex
		{ .position = { 0.0f, apex_y,  0.0f}, .normal = { 0.0f,  1.0f,  0.0f}, .uv = {0.5f, 0.5f}, .color = color_u32 },
		// Front face (Z+)
		{ .position = {-half, base_y,  half}, .normal = { 0.0f,  normal_y,  normal_xz}, .uv = {0.0f, 0.0f}, .color = color_u32 },
		{ .position = { half, base_y,  half}, .normal = { 0.0f,  normal_y,  normal_xz}, .uv = {1.0f, 0.0f}, .color = color_u32 },
		{ .position = { 0.0f, apex_y,  0.0f}, .normal = { 0.0f,  normal_y,  normal_xz}, .uv = {0.5f, 1.0f}, .color = color_u32 },
		// Right face (X+)
		{ .position = { half, base_y,  half}, .normal = { normal_xz,  normal_y,  0.0f}, .uv = {0.0f, 0.0f}, .color = color_u32 },
		{ .position = { half, base_y, -half}, .normal = { normal_xz,  normal_y,  0.0f}, .uv = {1.0f, 0.0f}, .color = color_u32 },
		{ .position = { 0.0f, apex_y,  0.0f}, .normal = { normal_xz,  normal_y,  0.0f}, .uv = {0.5f, 1.0f}, .color = color_u32 },
		// Back face (Z-)
		{ .position = { half, base_y, -half}, .normal = { 0.0f,  normal_y, -normal_xz}, .uv = {0.0f, 0.0f}, .color = color_u32 },
		{ .position = {-half, base_y, -half}, .normal = { 0.0f,  normal_y, -normal_xz}, .uv = {1.0f, 0.0f}, .color = color_u32 },
		{ .position = { 0.0f, apex_y,  0.0f}, .normal = { 0.0f,  normal_y, -normal_xz}, .uv = {0.5f, 1.0f}, .color = color_u32 },
		// Left face (X-)
		{ .position = {-half, base_y, -half}, .normal = {-normal_xz,  normal_y,  0.0f}, .uv = {0.0f, 0.0f}, .color = color_u32 },
		{ .position = {-half, base_y,  half}, .normal = {-normal_xz,  normal_y,  0.0f}, .uv = {1.0f, 0.0f}, .color = color_u32 },
		{ .position = { 0.0f, apex_y,  0.0f}, .normal = {-normal_xz,  normal_y,  0.0f}, .uv = {0.5f, 1.0f}, .color = color_u32 },
	};

	uint16_t indices[] = {
		0, 2, 1,  0, 3, 2,     // Base
		5, 6, 7,               // Front
		8, 9, 10,              // Right
		11, 12, 13,            // Back
		14, 15, 16,            // Left
	};

	skr_mesh_t mesh;
	skr_mesh_create(&su_vertex_type_pnuc, skr_index_fmt_u16, verts, 17, indices, 18, &mesh);
	return mesh;
}

skr_mesh_t su_mesh_create_quad(float width, float height, skr_vec3_t normal, bool double_sided, skr_vec4_t color) {
	float half_w = width * 0.5f;
	float half_h = height * 0.5f;

	// Determine axes based on normal
	skr_vec3_t tangent, bitangent;
	if (fabsf(normal.y) > 0.9f) {
		// Normal is mostly Y, quad on XZ plane
		tangent   = (skr_vec3_t){1.0f, 0.0f, 0.0f};
		bitangent = (skr_vec3_t){0.0f, 0.0f, 1.0f};
	} else if (fabsf(normal.z) > 0.9f) {
		// Normal is mostly Z, quad on XY plane
		tangent   = (skr_vec3_t){1.0f, 0.0f, 0.0f};
		bitangent = (skr_vec3_t){0.0f, 1.0f, 0.0f};
	} else {
		// Normal is mostly X, quad on YZ plane
		tangent   = (skr_vec3_t){0.0f, 1.0f, 0.0f};
		bitangent = (skr_vec3_t){0.0f, 0.0f, 1.0f};
	}

	int vert_count = double_sided ? 8 : 4;
	int idx_count  = double_sided ? 12 : 6;

	su_vertex_pnuc_t* verts = malloc(sizeof(su_vertex_pnuc_t) * vert_count);
	uint16_t*          inds  = malloc(sizeof(uint16_t) * idx_count);
	uint32_t           color_u32 = _color_vec4_to_u32(color);

	// Front face vertices
	for (int i = 0; i < 4; i++) {
		float u = (i & 1) ? 1.0f : 0.0f;
		float v = (i & 2) ? 1.0f : 0.0f;
		float x = (u - 0.5f) * width;
		float y = (v - 0.5f) * height;

		skr_vec3_t pos = {
			tangent.x * x + bitangent.x * y,
			tangent.y * x + bitangent.y * y,
			tangent.z * x + bitangent.z * y
		};

		verts[i] = (su_vertex_pnuc_t){
			.position = {pos.x, pos.y, pos.z},
			.normal   = normal,
			.uv       = {u, v},
			.color    = color_u32
		};
	}

	inds[0] = 3; inds[1] = 1; inds[2] = 0;
	inds[3] = 2; inds[4] = 3; inds[5] = 0;

	// Back face if double-sided
	if (double_sided) {
		skr_vec3_t back_normal = {-normal.x, -normal.y, -normal.z};
		for (int i = 0; i < 4; i++) {
			verts[i + 4] = verts[i];
			verts[i + 4].normal = back_normal;
		}
		inds[6] = 4; inds[7] = 6; inds[8] = 5;
		inds[9] = 6; inds[10] = 4; inds[11] = 7;
	}

	skr_mesh_t mesh;
	skr_mesh_create(&su_vertex_type_pnuc, skr_index_fmt_u16, verts, vert_count, inds, idx_count, &mesh);
	free(verts);
	free(inds);

	return mesh;
}

skr_mesh_t su_mesh_create_fullscreen_quad(void) {
	su_vertex_pnuc_t quad_vertices[] = {
		{ .position = {-1.0f, -1.0f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = { 1.0f, -1.0f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = 0xFFFFFFFF },
		{ .position = { 1.0f,  1.0f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = 0xFFFFFFFF },
		{ .position = {-1.0f,  1.0f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = 0xFFFFFFFF },
	};
	uint16_t quad_indices[] = { 0, 1, 2,  2, 3, 0 };
	skr_mesh_t mesh;
	skr_mesh_create(&su_vertex_type_pnuc, skr_index_fmt_u16, quad_vertices, 4, quad_indices, 6, &mesh);
	return mesh;
}

///////////////////////////////////////////////////////////////////////////////
// Texture Generation
///////////////////////////////////////////////////////////////////////////////

skr_tex_t su_tex_create_checkerboard(int32_t resolution, int32_t square_size, uint32_t color1, uint32_t color2, bool generate_mips) {
	uint32_t* pixels = malloc(resolution * resolution * sizeof(uint32_t));

	for (int y = 0; y < resolution; y++) {
		for (int x = 0; x < resolution; x++) {
			int checker_x = (x / square_size) % 2;
			int checker_y = (y / square_size) % 2;
			pixels[y * resolution + x] = (checker_x ^ checker_y) ? color1 : color2;
		}
	}

	skr_tex_flags_ flags = skr_tex_flags_readable;
	if (generate_mips) flags |= skr_tex_flags_gen_mips;

	skr_tex_t tex;
	skr_tex_create(
		skr_tex_fmt_rgba32_srgb,
		flags,
		su_sampler_linear_clamp,
		(skr_vec3i_t){resolution, resolution, 1},
		1, 0, pixels, &tex
	);

	free(pixels);

	if (generate_mips) {
		skr_tex_generate_mips(&tex, NULL);
	}

	return tex;
}

skr_tex_t su_tex_create_solid_color(uint32_t color) {
	skr_tex_t tex;
	skr_tex_create(
		skr_tex_fmt_rgba32_srgb,
		skr_tex_flags_readable,
		su_sampler_linear_clamp,
		(skr_vec3i_t){1, 1, 1},
		1, 1, &color, &tex
	);
	return tex;
}

///////////////////////////////////////////////////////////////////////////////
// File I/O
///////////////////////////////////////////////////////////////////////////////

bool su_file_read(const char* filename, void** out_data, size_t* out_size) {
	// Use SDL's RWops to read from Android assets
	SDL_RWops* rw = SDL_RWFromFile(filename, "rb");
	if (!rw) {
		su_log(su_log_critical, "Failed to open file '%s': %s", filename, SDL_GetError());
		return false;
	}

	Sint64 size = SDL_RWsize(rw);
	if (size < 0) {
		su_log(su_log_critical, "Failed to get size of file '%s': %s", filename, SDL_GetError());
		SDL_RWclose(rw);
		return false;
	}

	*out_size = (size_t)size;
	*out_data = malloc(*out_size);
	if (*out_data == NULL) {
		su_log(su_log_critical, "Failed to allocate %zu bytes for file '%s'", *out_size, filename);
		SDL_RWclose(rw);
		*out_size = 0;
		return false;
	}

	size_t bytes_read = SDL_RWread(rw, *out_data, 1, *out_size);
	SDL_RWclose(rw);

	if (bytes_read != *out_size) {
		su_log(su_log_critical, "Failed to read file '%s': expected %zu bytes, got %zu", filename, *out_size, bytes_read);
		free(*out_data);
		*out_data = NULL;
		*out_size = 0;
		return false;
	}

	return true;
}

///////////////////////////////////////////////////////////////////////////////
// Shader Loading
///////////////////////////////////////////////////////////////////////////////

skr_shader_t su_shader_load(const char* filename, const char* opt_name) {
	void*  shader_data = NULL;
	size_t shader_size = 0;

	skr_shader_t shader = {0};

	if (su_file_read(filename, &shader_data, &shader_size)) {
		skr_shader_create(shader_data, (uint32_t)shader_size, &shader);
		free(shader_data);

		if (opt_name && skr_shader_is_valid(&shader)) {
			skr_shader_set_name(&shader, opt_name);
		}
	}

	return shader;
}

///////////////////////////////////////////////////////////////////////////////
// Image Loading
///////////////////////////////////////////////////////////////////////////////

void* su_image_load(const char* filename, int32_t* opt_out_width, int32_t* opt_out_height, skr_tex_fmt_* opt_out_format, int32_t force_channels) {
	void*  file_data = NULL;
	size_t file_size = 0;

	if (!su_file_read(filename, &file_data, &file_size)) {
		return NULL;
	}

	void* pixels = su_image_load_from_memory(file_data, file_size, opt_out_width, opt_out_height, opt_out_format, force_channels);
	free(file_data);

	return pixels;
}

// Convert RGB floats to RGB9E5 shared exponent format
// Format: 9 bits each for R,G,B mantissa, 5 bits shared exponent
static uint32_t _float3_to_rgb9e5(float r, float g, float b) {
	// Clamp to valid range [0, MAX_RGB9E5]
	const float MAX_RGB9E5 = 65408.0f;  // (2^9 - 1) / 512 * 2^15
	r = fmaxf(0.0f, fminf(r, MAX_RGB9E5));
	g = fmaxf(0.0f, fminf(g, MAX_RGB9E5));
	b = fmaxf(0.0f, fminf(b, MAX_RGB9E5));

	// Find the maximum component to determine shared exponent
	float max_val = fmaxf(fmaxf(r, g), b);

	int32_t exp_shared;
	if (max_val < 1e-10f) {
		exp_shared = 0;
	} else {
		// Calculate exponent: floor(log2(max)) + 1 + bias(15)
		exp_shared = (int32_t)floorf(log2f(max_val)) + 1 + 15;
		exp_shared = exp_shared < 0 ? 0 : (exp_shared > 31 ? 31 : exp_shared);
	}

	// Calculate the divisor for this exponent
	float divisor = exp2f((float)(exp_shared - 15 - 9));

	// Convert to 9-bit mantissas
	uint32_t r_m = (uint32_t)(r / divisor + 0.5f);
	uint32_t g_m = (uint32_t)(g / divisor + 0.5f);
	uint32_t b_m = (uint32_t)(b / divisor + 0.5f);

	// Clamp mantissas to 9 bits
	r_m = r_m > 511 ? 511 : r_m;
	g_m = g_m > 511 ? 511 : g_m;
	b_m = b_m > 511 ? 511 : b_m;

	// Pack: R[8:0] | G[17:9] | B[26:18] | E[31:27]
	return r_m | (g_m << 9) | (b_m << 18) | ((uint32_t)exp_shared << 27);
}

void* su_image_load_from_memory(const void* data, size_t size, int32_t* opt_out_width, int32_t* opt_out_height, skr_tex_fmt_* opt_out_format, int32_t force_channels) {
	int   width, height, channels;
	void* pixels = NULL;
	bool  is_hdr = stbi_is_hdr_from_memory((const unsigned char*)data, (int)size);

	if (is_hdr) {
		// Load as float, then convert to RGB9E5
		float* hdr_pixels = stbi_loadf_from_memory((const unsigned char*)data, (int)size, &width, &height, &channels, 3);
		if (hdr_pixels) {
			int32_t   pixel_count = width * height;
			uint32_t* rgb9e5      = malloc(pixel_count * sizeof(uint32_t));

			for (int32_t i = 0; i < pixel_count; i++) {
				rgb9e5[i] = _float3_to_rgb9e5(hdr_pixels[i * 3], hdr_pixels[i * 3 + 1], hdr_pixels[i * 3 + 2]);
			}

			stbi_image_free(hdr_pixels);
			pixels = rgb9e5;
			if (opt_out_format) *opt_out_format = skr_tex_fmt_rgb9e5;
		}
	} else {
		pixels = stbi_load_from_memory((const unsigned char*)data, (int)size, &width, &height, &channels, force_channels);
		if (pixels && opt_out_format) *opt_out_format = skr_tex_fmt_rgba32_srgb;
	}

	if (pixels) {
		if (opt_out_width)  *opt_out_width  = width;
		if (opt_out_height) *opt_out_height = height;
	}

	return pixels;
}

void su_image_free(void* pixels) {
	// Both stbi and malloc use free() on most platforms
	free(pixels);
}

///////////////////////////////////////////////////////////////////////////////
// Utility Functions
///////////////////////////////////////////////////////////////////////////////

float su_hash_f(int32_t position, uint32_t seed) {
	// Bit noise constants from http://www.isthe.com/chongo/tech/comp/fnv/
	const uint32_t BIT_NOISE1 = 0xB5297A4D;
	const uint32_t BIT_NOISE2 = 0x68E31DA4;
	const uint32_t BIT_NOISE3 = 0x1B56C4E9;

	uint32_t mangled = (uint32_t)position;
	mangled *= BIT_NOISE1;
	mangled ^= (seed);
	mangled ^= (mangled >> 8);
	mangled += BIT_NOISE2;
	mangled ^= (mangled << 8);
	mangled *= BIT_NOISE3;
	mangled ^= (mangled >> 8);

	return (float)mangled / 4294967296.0f;  // Normalize to [0.0, 1.0]
}

///////////////////////////////////////////////////////////////////////////////
// Asset Loading Thread
///////////////////////////////////////////////////////////////////////////////

#define SU_MAX_PENDING_LOADS 32

typedef enum {
	_su_load_type_gltf,
} _su_load_type_;

typedef struct _su_load_request_t {
	_su_load_type_ type;
	void*          asset;  // Pointer to su_gltf_t or other asset type
} _su_load_request_t;

typedef struct {
	thrd_t thread;
	mtx_t  queue_mutex;
	bool   running;

	_su_load_request_t requests[SU_MAX_PENDING_LOADS];
	int32_t            request_head;  // Next slot to write
	int32_t            request_tail;  // Next slot to read
} _su_asset_loader_t;

static _su_asset_loader_t _su_loader = {0};

// Forward declarations
static void _su_gltf_load_sync(su_gltf_t* gltf);

static int32_t _su_loader_thread(void* arg) {
	(void)arg;

	// Initialize this thread for sk_renderer
	skr_thread_init();

	su_log(su_log_info, "Asset loader thread started");

	while (_su_loader.running) {
		_su_load_request_t request = {0};
		bool               has_request = false;

		// Check for pending requests
		mtx_lock(&_su_loader.queue_mutex);
		if (_su_loader.request_head != _su_loader.request_tail) {
			request     = _su_loader.requests[_su_loader.request_tail];
			_su_loader.request_tail = (_su_loader.request_tail + 1) % SU_MAX_PENDING_LOADS;
			has_request = true;
		}
		mtx_unlock(&_su_loader.queue_mutex);

		if (has_request) {
			switch (request.type) {
			case _su_load_type_gltf:
				_su_gltf_load_sync((su_gltf_t*)request.asset);
				break;
			}
		} else {
			// Sleep briefly to avoid busy-waiting
			thrd_sleep(&(struct timespec){.tv_nsec = 10000000}, NULL);  // 10ms
		}
	}

	su_log(su_log_info, "Asset loader thread stopped");
	skr_thread_shutdown();
	return 0;
}

static void _su_loader_enqueue(_su_load_type_ type, void* asset) {
	mtx_lock(&_su_loader.queue_mutex);

	int32_t next_head = (_su_loader.request_head + 1) % SU_MAX_PENDING_LOADS;
	if (next_head != _su_loader.request_tail) {
		_su_loader.requests[_su_loader.request_head] = (_su_load_request_t){
			.type  = type,
			.asset = asset,
		};
		_su_loader.request_head = next_head;
	} else {
		su_log(su_log_warning, "Asset loader queue full, request dropped");
	}

	mtx_unlock(&_su_loader.queue_mutex);
}

void su_initialize(void) {
	// Initialize vertex types
	_su_vertex_types_init();

	// Start asset loading thread
	mtx_init(&_su_loader.queue_mutex, mtx_plain);
	_su_loader.running      = true;
	_su_loader.request_head = 0;
	_su_loader.request_tail = 0;
	thrd_create(&_su_loader.thread, _su_loader_thread, NULL);

	su_log(su_log_info, "Scene utilities initialized");
}

void su_shutdown(void) {
	// Stop loading thread
	_su_loader.running = false;
	thrd_join(_su_loader.thread, NULL);
	mtx_destroy(&_su_loader.queue_mutex);

	su_log(su_log_info, "Scene utilities shut down");
}

///////////////////////////////////////////////////////////////////////////////
// GLTF Loading
///////////////////////////////////////////////////////////////////////////////

#define SU_GLTF_MAX_MESHES   64
#define SU_GLTF_MAX_TEXTURES 32

// Texture types for PBR materials
typedef enum {
	_su_gltf_tex_albedo = 0,
	_su_gltf_tex_metallic_roughness,
	_su_gltf_tex_normal,
	_su_gltf_tex_occlusion,
	_su_gltf_tex_emissive,
	_su_gltf_tex_count
} _su_gltf_tex_type_;

// Per-mesh material data extracted from GLTF
typedef struct {
	int32_t    texture_indices[_su_gltf_tex_count];  // Indices per texture type, -1 if none
	float      metallic_factor;
	float      roughness_factor;
	skr_vec4_t base_color_factor;
	skr_vec3_t emissive_factor;
	skr_vec4_t tex_trans;  // Texture transform: {offset.x, offset.y, scale.x, scale.y}
} _su_gltf_material_data_t;

struct su_gltf_t {
	su_gltf_state_  state;
	char            filepath[256];
	skr_shader_t*   shader;  // Borrowed reference

	// GPU resources (created on loader thread)
	skr_mesh_t      meshes   [SU_GLTF_MAX_MESHES];
	skr_material_t  materials[SU_GLTF_MAX_MESHES];
	float4x4        transforms[SU_GLTF_MAX_MESHES];
	su_bounds_t     mesh_bounds[SU_GLTF_MAX_MESHES];  // Per-mesh bounds (world space)
	su_bounds_t     bounds;                           // Overall model bounds
	int32_t         mesh_count;

	skr_tex_t       textures[SU_GLTF_MAX_TEXTURES];
	int32_t         texture_count;

	// Fallback textures (created on loader thread)
	skr_tex_t       white_texture;
	skr_tex_t       black_texture;
	skr_tex_t       default_metal_texture;
};

// Helper to read vertex attribute with default value
static void _su_gltf_read_attribute(cgltf_accessor* accessor, int32_t index, float* out_data, int32_t component_count, const float* default_value) {
	if (accessor && cgltf_accessor_read_float(accessor, index, out_data, component_count)) {
		return;
	}
	for (int32_t i = 0; i < component_count; i++) {
		out_data[i] = default_value[i];
	}
}

// Helper to calculate node transform from GLTF node
static float4x4 _su_gltf_node_transform(cgltf_node* node) {
	if (node->has_matrix) {
		// cgltf uses column-major, float_math uses row-major, so transpose
		float4x4 m;
		memcpy(&m, node->matrix, sizeof(float) * 16);
		return float4x4_transpose(m);
	}

	// Build from TRS
	float3 pos   = node->has_translation ? (float3){node->translation[0], node->translation[1], node->translation[2]} : (float3){0,0,0};
	float4 rot   = node->has_rotation    ? (float4){node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]} : (float4){0,0,0,1};
	float3 scale = node->has_scale       ? (float3){node->scale[0], node->scale[1], node->scale[2]} : (float3){1,1,1};
	return float4x4_trs(pos, rot, scale);
}

// Helper to find texture index from image pointer
static int32_t _su_gltf_find_texture_index(cgltf_data* data, cgltf_image* img) {
	if (!img) return -1;
	for (size_t t = 0; t < data->images_count; t++) {
		if ((void*)img == (void*)&data->images[t]) {
			return (int32_t)t;
		}
	}
	return -1;
}

// Extract mesh primitives from a GLTF node
static void _su_gltf_extract_node(
	cgltf_data*               data,
	cgltf_node*               node,
	float4x4                  parent_transform,
	su_gltf_t*                gltf,
	_su_gltf_material_data_t* out_mat_data
) {
	float4x4 local_transform = _su_gltf_node_transform(node);
	float4x4 world_transform = float4x4_mul(parent_transform, local_transform);

	if (node->mesh && gltf->mesh_count < SU_GLTF_MAX_MESHES) {
		cgltf_mesh* mesh = node->mesh;

		// Process first primitive only (for simplicity)
		if (mesh->primitives_count > 0) {
			cgltf_primitive* prim = &mesh->primitives[0];
			if (prim->type != cgltf_primitive_type_triangles) goto recurse;

			int32_t mesh_idx = gltf->mesh_count;
			gltf->transforms[mesh_idx] = world_transform;

			// Initialize material data
			_su_gltf_material_data_t* mat_data = &out_mat_data[mesh_idx];
			for (int32_t i = 0; i < _su_gltf_tex_count; i++) mat_data->texture_indices[i] = -1;
			mat_data->metallic_factor   = 1.0f;
			mat_data->roughness_factor  = 1.0f;
			mat_data->base_color_factor = (skr_vec4_t){1.0f, 1.0f, 1.0f, 1.0f};
			mat_data->emissive_factor   = (skr_vec3_t){0.0f, 0.0f, 0.0f};
			mat_data->tex_trans         = (skr_vec4_t){0.0f, 0.0f, 1.0f, 1.0f};  // Default: no offset, scale 1

			// Find accessors
			cgltf_accessor* pos_accessor   = NULL;
			cgltf_accessor* norm_accessor  = NULL;
			cgltf_accessor* uv_accessor    = NULL;
			cgltf_accessor* color_accessor = NULL;

			for (size_t i = 0; i < prim->attributes_count; i++) {
				cgltf_attribute* attr = &prim->attributes[i];
				if      (attr->type == cgltf_attribute_type_position)                     pos_accessor   = attr->data;
				else if (attr->type == cgltf_attribute_type_normal)                       norm_accessor  = attr->data;
				else if (attr->type == cgltf_attribute_type_texcoord && attr->index == 0) uv_accessor    = attr->data;
				else if (attr->type == cgltf_attribute_type_color    && attr->index == 0) color_accessor = attr->data;
			}

			if (!pos_accessor) goto recurse;

			// Build vertex data and compute bounds
			int32_t           vertex_count = (int32_t)pos_accessor->count;
			su_vertex_pnuc_t* vertices     = calloc(vertex_count, sizeof(su_vertex_pnuc_t));

			// Initialize mesh bounds to first vertex (will expand)
			su_bounds_t local_bounds = {
				.min = { FLT_MAX,  FLT_MAX,  FLT_MAX},
				.max = {-FLT_MAX, -FLT_MAX, -FLT_MAX}
			};

			for (int32_t v = 0; v < vertex_count; v++) {
				su_vertex_pnuc_t* vert = &vertices[v];
				float d[4];

				_su_gltf_read_attribute(pos_accessor,   v, d, 3, (float[]){0, 0, 0});
				vert->position = (skr_vec3_t){d[0], d[1], d[2]};

				// Expand local bounds
				if (d[0] < local_bounds.min.x) local_bounds.min.x = d[0];
				if (d[1] < local_bounds.min.y) local_bounds.min.y = d[1];
				if (d[2] < local_bounds.min.z) local_bounds.min.z = d[2];
				if (d[0] > local_bounds.max.x) local_bounds.max.x = d[0];
				if (d[1] > local_bounds.max.y) local_bounds.max.y = d[1];
				if (d[2] > local_bounds.max.z) local_bounds.max.z = d[2];

				_su_gltf_read_attribute(norm_accessor,  v, d, 3, (float[]){0, 1, 0});
				vert->normal = (skr_vec3_t){d[0], d[1], d[2]};

				_su_gltf_read_attribute(uv_accessor,    v, d, 2, (float[]){0, 0});
				vert->uv = (skr_vec2_t){d[0], d[1]};

				_su_gltf_read_attribute(color_accessor, v, d, 4, (float[]){1, 1, 1, 1});
				uint8_t r = (uint8_t)(d[0] * 255.0f);
				uint8_t g = (uint8_t)(d[1] * 255.0f);
				uint8_t b = (uint8_t)(d[2] * 255.0f);
				uint8_t a = (uint8_t)(d[3] * 255.0f);
				vert->color = (a << 24) | (b << 16) | (g << 8) | r;
			}

			// Transform bounds to world space (transform all 8 corners and find new AABB)
			su_bounds_t world_bounds = {
				.min = { FLT_MAX,  FLT_MAX,  FLT_MAX},
				.max = {-FLT_MAX, -FLT_MAX, -FLT_MAX}
			};
			for (int32_t corner = 0; corner < 8; corner++) {
				float3 local_corner = {
					(corner & 1) ? local_bounds.max.x : local_bounds.min.x,
					(corner & 2) ? local_bounds.max.y : local_bounds.min.y,
					(corner & 4) ? local_bounds.max.z : local_bounds.min.z,
				};
				float3 world_corner = float4x4_transform_pt(world_transform, local_corner);
				if (world_corner.x < world_bounds.min.x) world_bounds.min.x = world_corner.x;
				if (world_corner.y < world_bounds.min.y) world_bounds.min.y = world_corner.y;
				if (world_corner.z < world_bounds.min.z) world_bounds.min.z = world_corner.z;
				if (world_corner.x > world_bounds.max.x) world_bounds.max.x = world_corner.x;
				if (world_corner.y > world_bounds.max.y) world_bounds.max.y = world_corner.y;
				if (world_corner.z > world_bounds.max.z) world_bounds.max.z = world_corner.z;
			}
			gltf->mesh_bounds[mesh_idx] = world_bounds;

			// Build index data - use 32-bit indices if vertex count exceeds 16-bit range
			int32_t index_count  = prim->indices ? (int32_t)prim->indices->count : 0;
			bool    use_32bit    = vertex_count > 65535;
			void*   indices      = NULL;

			if (index_count > 0) {
				if (use_32bit) {
					uint32_t* idx32 = malloc(index_count * sizeof(uint32_t));
					for (int32_t i = 0; i < index_count; i++) {
						idx32[i] = (uint32_t)cgltf_accessor_read_index(prim->indices, i);
					}
					indices = idx32;
				} else {
					uint16_t* idx16 = malloc(index_count * sizeof(uint16_t));
					for (int32_t i = 0; i < index_count; i++) {
						idx16[i] = (uint16_t)cgltf_accessor_read_index(prim->indices, i);
					}
					indices = idx16;
				}
			}

			// Create GPU mesh
			skr_index_fmt_ idx_fmt = use_32bit ? skr_index_fmt_u32 : skr_index_fmt_u16;
			skr_mesh_create(&su_vertex_type_pnuc, idx_fmt, vertices, vertex_count, indices, index_count, &gltf->meshes[mesh_idx]);
			char mesh_name[64];
			snprintf(mesh_name, sizeof(mesh_name), "gltf_mesh_%d", mesh_idx);
			skr_mesh_set_name(&gltf->meshes[mesh_idx], mesh_name);

			free(vertices);
			free(indices);

			// Extract material properties
			if (prim->material) {
				cgltf_material*              mat = prim->material;
				cgltf_pbr_metallic_roughness* pbr = &mat->pbr_metallic_roughness;

				mat_data->metallic_factor   = pbr->metallic_factor;
				mat_data->roughness_factor  = pbr->roughness_factor;
				mat_data->base_color_factor = (skr_vec4_t){pbr->base_color_factor[0], pbr->base_color_factor[1], pbr->base_color_factor[2], pbr->base_color_factor[3]};
				mat_data->emissive_factor   = (skr_vec3_t){mat->emissive_factor[0], mat->emissive_factor[1], mat->emissive_factor[2]};

				// Extract texture transform from base color texture (KHR_texture_transform)
				if (pbr->base_color_texture.has_transform) {
					cgltf_texture_transform* t = &pbr->base_color_texture.transform;
					mat_data->tex_trans = (skr_vec4_t){t->offset[0], t->offset[1], t->scale[0], t->scale[1]};
				}

				// Texture indices
				if (pbr->base_color_texture.texture)         mat_data->texture_indices[_su_gltf_tex_albedo]             = _su_gltf_find_texture_index(data, pbr->base_color_texture.texture->image);
				if (pbr->metallic_roughness_texture.texture) mat_data->texture_indices[_su_gltf_tex_metallic_roughness] = _su_gltf_find_texture_index(data, pbr->metallic_roughness_texture.texture->image);
				if (mat->normal_texture.texture)             mat_data->texture_indices[_su_gltf_tex_normal]             = _su_gltf_find_texture_index(data, mat->normal_texture.texture->image);
				if (mat->occlusion_texture.texture)          mat_data->texture_indices[_su_gltf_tex_occlusion]          = _su_gltf_find_texture_index(data, mat->occlusion_texture.texture->image);
				if (mat->emissive_texture.texture)           mat_data->texture_indices[_su_gltf_tex_emissive]           = _su_gltf_find_texture_index(data, mat->emissive_texture.texture->image);
			}

			gltf->mesh_count++;
		}
	}

recurse:
	for (size_t i = 0; i < node->children_count; i++) {
		_su_gltf_extract_node(data, node->children[i], world_transform, gltf, out_mat_data);
	}
}

// Load texture from various GLTF sources (GLTF textures are always LDR 8-bit)
static bool _su_gltf_load_texture_data(cgltf_data* data, cgltf_options* options, const char* base_path, int32_t tex_idx, unsigned char** out_pixels, int32_t* out_width, int32_t* out_height) {
	if (tex_idx < 0 || tex_idx >= (int32_t)data->images_count) return false;

	cgltf_image* img = &data->images[tex_idx];

	// Try buffer view (embedded)
	if (img->buffer_view) {
		cgltf_buffer_view* view     = img->buffer_view;
		unsigned char*     img_data = (unsigned char*)view->buffer->data + view->offset;
		*out_pixels = (unsigned char*)su_image_load_from_memory(img_data, view->size, out_width, out_height, NULL, 4);
		return *out_pixels != NULL;
	}

	// Try data URI
	if (img->uri && strncmp(img->uri, "data:", 5) == 0) {
		char* comma = strchr(img->uri, ',');
		if (comma && comma - img->uri >= 7 && strncmp(comma - 7, ";base64", 7) == 0) {
			char*  base64_start = comma + 1;
			size_t base64_len   = strlen(base64_start);
			size_t base64_size  = 3 * (base64_len / 4);
			if (base64_len >= 1 && base64_start[base64_len - 1] == '=') base64_size -= 1;
			if (base64_len >= 2 && base64_start[base64_len - 2] == '=') base64_size -= 1;

			void* buffer = NULL;
			if (cgltf_load_buffer_base64(options, base64_size, base64_start, &buffer) == cgltf_result_success && buffer) {
				*out_pixels = (unsigned char*)su_image_load_from_memory((unsigned char*)buffer, base64_size, out_width, out_height, NULL, 4);
				free(buffer);
				return *out_pixels != NULL;
			}
		}
		return false;
	}

	// Try external file
	if (img->uri) {
		char texture_path[512];
		if (base_path[0] != '\0') {
			snprintf(texture_path, sizeof(texture_path), "%s%s", base_path, img->uri);
		} else {
			snprintf(texture_path, sizeof(texture_path), "%s", img->uri);
		}
		*out_pixels = (unsigned char*)su_image_load(texture_path, out_width, out_height, NULL, 4);
		return *out_pixels != NULL;
	}

	return false;
}

// Synchronous GLTF loading (runs on loader thread)
static void _su_gltf_load_sync(su_gltf_t* gltf) {
	su_log(su_log_info, "GLTF: Loading %s", gltf->filepath);

	// Extract directory path
	char base_path[256] = "";
	const char* last_slash = strrchr(gltf->filepath, '/');
	if (last_slash) {
		size_t dir_len = last_slash - gltf->filepath + 1;
		if (dir_len < sizeof(base_path)) {
			strncpy(base_path, gltf->filepath, dir_len);
			base_path[dir_len] = '\0';
		}
	}

	// Load and parse file
	void*  file_data = NULL;
	size_t file_size = 0;
	if (!su_file_read(gltf->filepath, &file_data, &file_size)) {
		su_log(su_log_critical, "GLTF: Failed to read file");
		gltf->state = su_gltf_state_failed;
		return;
	}

	cgltf_options options = {0};
	cgltf_data*   data    = NULL;
	if (cgltf_parse(&options, file_data, file_size, &data) != cgltf_result_success) {
		su_log(su_log_critical, "GLTF: Failed to parse");
		free(file_data);
		gltf->state = su_gltf_state_failed;
		return;
	}

	if (cgltf_load_buffers(&options, data, gltf->filepath) != cgltf_result_success) {
		su_log(su_log_critical, "GLTF: Failed to load buffers");
		cgltf_free(data);
		free(file_data);
		gltf->state = su_gltf_state_failed;
		return;
	}

	// Create fallback textures
	gltf->white_texture         = su_tex_create_solid_color(0xFFFFFFFF);
	gltf->black_texture         = su_tex_create_solid_color(0xFF000000);
	gltf->default_metal_texture = su_tex_create_solid_color(0xFFFFFFFF);
	skr_tex_set_name(&gltf->white_texture,         "gltf_white_fallback");
	skr_tex_set_name(&gltf->black_texture,         "gltf_black_fallback");
	skr_tex_set_name(&gltf->default_metal_texture, "gltf_metal_fallback");

	// Extract meshes
	_su_gltf_material_data_t mat_data[SU_GLTF_MAX_MESHES] = {0};
	if (data->scene && data->scene->nodes_count > 0) {
		for (size_t i = 0; i < data->scene->nodes_count; i++) {
			_su_gltf_extract_node(data, data->scene->nodes[i], float4x4_identity(), gltf, mat_data);
		}
	}

	// Compute overall model bounds from all mesh bounds
	gltf->bounds.min = (float3){ FLT_MAX,  FLT_MAX,  FLT_MAX};
	gltf->bounds.max = (float3){-FLT_MAX, -FLT_MAX, -FLT_MAX};
	for (int32_t i = 0; i < gltf->mesh_count; i++) {
		su_bounds_t* mb = &gltf->mesh_bounds[i];
		if (mb->min.x < gltf->bounds.min.x) gltf->bounds.min.x = mb->min.x;
		if (mb->min.y < gltf->bounds.min.y) gltf->bounds.min.y = mb->min.y;
		if (mb->min.z < gltf->bounds.min.z) gltf->bounds.min.z = mb->min.z;
		if (mb->max.x > gltf->bounds.max.x) gltf->bounds.max.x = mb->max.x;
		if (mb->max.y > gltf->bounds.max.y) gltf->bounds.max.y = mb->max.y;
		if (mb->max.z > gltf->bounds.max.z) gltf->bounds.max.z = mb->max.z;
	}

	// Create materials with default textures
	for (int32_t i = 0; i < gltf->mesh_count; i++) {
		_su_gltf_material_data_t* md = &mat_data[i];

		skr_material_create((skr_material_info_t){
			.shader     = gltf->shader,
			.cull       = skr_cull_back,
			.write_mask = skr_write_default,
			.depth_test = skr_compare_less,
		}, &gltf->materials[i]);

		// Set default fallback textures
		skr_material_set_tex(&gltf->materials[i], "albedo_tex",    &gltf->white_texture);
		skr_material_set_tex(&gltf->materials[i], "emission_tex",  &gltf->black_texture);
		skr_material_set_tex(&gltf->materials[i], "metal_tex",     &gltf->default_metal_texture);
		skr_material_set_tex(&gltf->materials[i], "occlusion_tex", &gltf->white_texture);

		// Set material parameters
		skr_material_set_param(&gltf->materials[i], "color",           sksc_shader_var_float, 4, &md->base_color_factor);
		skr_vec4_t emission = {md->emissive_factor.x, md->emissive_factor.y, md->emissive_factor.z, 1.0f};
		skr_material_set_param(&gltf->materials[i], "emission_factor", sksc_shader_var_float, 4, &emission);
		skr_material_set_param(&gltf->materials[i], "tex_trans",       sksc_shader_var_float, 4, &md->tex_trans);
		skr_material_set_param(&gltf->materials[i], "metallic",        sksc_shader_var_float, 1, &md->metallic_factor);
		skr_material_set_param(&gltf->materials[i], "roughness",       sksc_shader_var_float, 1, &md->roughness_factor);
	}

	// Meshes ready - can start rendering with default materials
	gltf->state = su_gltf_state_ready;

	// Load textures and bind to materials
	bool texture_loaded[SU_GLTF_MAX_TEXTURES] = {0};
	for (int32_t m = 0; m < gltf->mesh_count; m++) {
		_su_gltf_material_data_t* md = &mat_data[m];

		for (int32_t tex_type = 0; tex_type < _su_gltf_tex_count; tex_type++) {
			int32_t tex_idx = md->texture_indices[tex_type];
			if (tex_idx < 0) continue;

			// Load texture if not already loaded
			if (!texture_loaded[tex_idx]) {
				unsigned char* pixels = NULL;
				int32_t        width  = 0, height = 0;

				if (_su_gltf_load_texture_data(data, &options, base_path, tex_idx, &pixels, &width, &height)) {
					skr_tex_create(
						skr_tex_fmt_rgba32_srgb,
						skr_tex_flags_readable | skr_tex_flags_gen_mips,
						su_sampler_linear_wrap,
						(skr_vec3i_t){width, height, 1},
						1, 0, pixels, &gltf->textures[tex_idx]
					);

					char tex_name[64];
					snprintf(tex_name, sizeof(tex_name), "gltf_tex_%d", tex_idx);
					skr_tex_set_name(&gltf->textures[tex_idx], tex_name);
					skr_tex_generate_mips(&gltf->textures[tex_idx], NULL);

					su_image_free(pixels);
					texture_loaded[tex_idx] = true;
					gltf->texture_count++;
				}
			}

			// Bind texture to material if loaded
			if (texture_loaded[tex_idx]) {
				const char* bind_names[] = {"albedo_tex", "metal_tex", NULL, "occlusion_tex", "emission_tex"};
				const char* bind_name    = bind_names[tex_type];
				if (bind_name) {
					skr_material_set_tex(&gltf->materials[m], bind_name, &gltf->textures[tex_idx]);
				}
			}
		}
	}

	cgltf_free(data);
	free(file_data);

	su_log(su_log_info, "GLTF: Ready (%d meshes, %d textures)", gltf->mesh_count, gltf->texture_count);
}

su_gltf_t* su_gltf_load(const char* filename, skr_shader_t* shader) {
	su_gltf_t* gltf = calloc(1, sizeof(su_gltf_t));
	if (!gltf) return NULL;

	gltf->state  = su_gltf_state_loading;
	gltf->shader = shader;
	snprintf(gltf->filepath, sizeof(gltf->filepath), "%s", filename);

	// Enqueue for async loading
	_su_loader_enqueue(_su_load_type_gltf, gltf);

	return gltf;
}

void su_gltf_destroy(su_gltf_t* gltf) {
	if (!gltf) return;

	// TODO: If still loading, need to wait for loader thread to finish with this asset
	// For now, sk_renderer's deferred destruction handles in-flight resources

	for (int32_t i = 0; i < gltf->mesh_count; i++) {
		skr_mesh_destroy    (&gltf->meshes[i]);
		skr_material_destroy(&gltf->materials[i]);
	}

	for (int32_t i = 0; i < SU_GLTF_MAX_TEXTURES; i++) {
		skr_tex_destroy(&gltf->textures[i]);
	}

	skr_tex_destroy(&gltf->white_texture);
	skr_tex_destroy(&gltf->black_texture);
	skr_tex_destroy(&gltf->default_metal_texture);

	free(gltf);
}

su_gltf_state_ su_gltf_get_state(su_gltf_t* gltf) {
	return gltf ? gltf->state : su_gltf_state_failed;
}

su_bounds_t su_gltf_get_bounds(su_gltf_t* gltf) {
	if (!gltf || gltf->state != su_gltf_state_ready) {
		return (su_bounds_t){
			.min = {0, 0, 0},
			.max = {0, 0, 0}
		};
	}
	return gltf->bounds;
}

void su_gltf_add_to_render_list(su_gltf_t* gltf, skr_render_list_t* list, const float4x4* opt_transform) {
	if (!gltf || gltf->state != su_gltf_state_ready) return;

	for (int32_t i = 0; i < gltf->mesh_count; i++) {
		float4x4 world = gltf->transforms[i];
		if (opt_transform) {
			world = float4x4_mul(*opt_transform, world);
		}
		skr_render_list_add(list, &gltf->meshes[i], &gltf->materials[i], &world, sizeof(float4x4), 1);
	}
}

