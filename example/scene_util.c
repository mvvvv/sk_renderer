// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "scene_util.h"
#include "app.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

///////////////////////////////////////////////////////////////////////////////
// Common Texture Samplers
///////////////////////////////////////////////////////////////////////////////

const skr_tex_sampler_t skr_sampler_linear_clamp = {
	.sample  = skr_tex_sample_linear,
	.address = skr_tex_address_clamp
};

const skr_tex_sampler_t skr_sampler_linear_wrap = {
	.sample  = skr_tex_sample_linear,
	.address = skr_tex_address_wrap
};

const skr_tex_sampler_t skr_sampler_point_clamp = {
	.sample  = skr_tex_sample_point,
	.address = skr_tex_address_clamp
};

///////////////////////////////////////////////////////////////////////////////
// Standard Vertex Types
///////////////////////////////////////////////////////////////////////////////

skr_vert_type_t skr_vertex_type_pnuc = {0};

void skr_vertex_types_init(void) {
	skr_vertex_type_pnuc = skr_vert_type_create(
		(skr_vert_component_t[]){
			{ .format = skr_vertex_fmt_f32, .count = 4, .semantic = skr_semantic_position, .semantic_slot = 0 },
			{ .format = skr_vertex_fmt_f32, .count = 3, .semantic = skr_semantic_normal,   .semantic_slot = 0 },
			{ .format = skr_vertex_fmt_f32, .count = 2, .semantic = skr_semantic_texcoord, .semantic_slot = 0 },
			{ .format = skr_vertex_fmt_f32, .count = 4, .semantic = skr_semantic_color,    .semantic_slot = 0 }}, 4);
}

///////////////////////////////////////////////////////////////////////////////
// Mesh Generation
///////////////////////////////////////////////////////////////////////////////

skr_mesh_t skr_mesh_create_sphere(int32_t segments, int32_t rings, float radius, skr_vec4_t color) {
	const int vert_count = (rings + 1) * (segments + 1);
	const int idx_count  = rings * segments * 6;

	skr_vertex_pnuc_t* verts = malloc(sizeof(skr_vertex_pnuc_t) * vert_count);
	uint16_t*          inds  = malloc(sizeof(uint16_t) * idx_count);

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

			verts[v_idx++] = (skr_vertex_pnuc_t){
				.position = {x * radius, y * radius, z * radius, 1.0f},
				.normal   = {x, y, z},
				.uv       = {u, v},
				.color    = color
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

	skr_mesh_t mesh = skr_mesh_create(&skr_vertex_type_pnuc, skr_index_fmt_u16, verts, vert_count, inds, idx_count);
	free(verts);
	free(inds);

	return mesh;
}

skr_mesh_t skr_mesh_create_cube(float size, const skr_vec4_t* opt_face_colors) {
	float half = size * 0.5f;

	// Default to white if no colors provided
	skr_vec4_t white = {1.0f, 1.0f, 1.0f, 1.0f};
	skr_vec4_t colors[6];
	for (int i = 0; i < 6; i++) {
		colors[i] = opt_face_colors ? opt_face_colors[i] : white;
	}

	skr_vertex_pnuc_t verts[24] = {
		// Front face (Z+)
		{ .position = {-half, -half,  half, 1.0f}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {0.0f, 0.0f}, .color = colors[0] },
		{ .position = { half, -half,  half, 1.0f}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {1.0f, 0.0f}, .color = colors[0] },
		{ .position = { half,  half,  half, 1.0f}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {1.0f, 1.0f}, .color = colors[0] },
		{ .position = {-half,  half,  half, 1.0f}, .normal = { 0.0f,  0.0f,  1.0f}, .uv = {0.0f, 1.0f}, .color = colors[0] },
		// Back face (Z-)
		{ .position = { half, -half, -half, 1.0f}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {0.0f, 0.0f}, .color = colors[1] },
		{ .position = {-half, -half, -half, 1.0f}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {1.0f, 0.0f}, .color = colors[1] },
		{ .position = {-half,  half, -half, 1.0f}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {1.0f, 1.0f}, .color = colors[1] },
		{ .position = { half,  half, -half, 1.0f}, .normal = { 0.0f,  0.0f, -1.0f}, .uv = {0.0f, 1.0f}, .color = colors[1] },
		// Top face (Y+)
		{ .position = {-half,  half,  half, 1.0f}, .normal = { 0.0f,  1.0f,  0.0f}, .uv = {0.0f, 0.0f}, .color = colors[2] },
		{ .position = { half,  half,  half, 1.0f}, .normal = { 0.0f,  1.0f,  0.0f}, .uv = {1.0f, 0.0f}, .color = colors[2] },
		{ .position = { half,  half, -half, 1.0f}, .normal = { 0.0f,  1.0f,  0.0f}, .uv = {1.0f, 1.0f}, .color = colors[2] },
		{ .position = {-half,  half, -half, 1.0f}, .normal = { 0.0f,  1.0f,  0.0f}, .uv = {0.0f, 1.0f}, .color = colors[2] },
		// Bottom face (Y-)
		{ .position = {-half, -half, -half, 1.0f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {0.0f, 0.0f}, .color = colors[3] },
		{ .position = { half, -half, -half, 1.0f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {1.0f, 0.0f}, .color = colors[3] },
		{ .position = { half, -half,  half, 1.0f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {1.0f, 1.0f}, .color = colors[3] },
		{ .position = {-half, -half,  half, 1.0f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {0.0f, 1.0f}, .color = colors[3] },
		// Right face (X+)
		{ .position = { half, -half,  half, 1.0f}, .normal = { 1.0f,  0.0f,  0.0f}, .uv = {0.0f, 0.0f}, .color = colors[4] },
		{ .position = { half, -half, -half, 1.0f}, .normal = { 1.0f,  0.0f,  0.0f}, .uv = {1.0f, 0.0f}, .color = colors[4] },
		{ .position = { half,  half, -half, 1.0f}, .normal = { 1.0f,  0.0f,  0.0f}, .uv = {1.0f, 1.0f}, .color = colors[4] },
		{ .position = { half,  half,  half, 1.0f}, .normal = { 1.0f,  0.0f,  0.0f}, .uv = {0.0f, 1.0f}, .color = colors[4] },
		// Left face (X-)
		{ .position = {-half, -half, -half, 1.0f}, .normal = {-1.0f,  0.0f,  0.0f}, .uv = {0.0f, 0.0f}, .color = colors[5] },
		{ .position = {-half, -half,  half, 1.0f}, .normal = {-1.0f,  0.0f,  0.0f}, .uv = {1.0f, 0.0f}, .color = colors[5] },
		{ .position = {-half,  half,  half, 1.0f}, .normal = {-1.0f,  0.0f,  0.0f}, .uv = {1.0f, 1.0f}, .color = colors[5] },
		{ .position = {-half,  half, -half, 1.0f}, .normal = {-1.0f,  0.0f,  0.0f}, .uv = {0.0f, 1.0f}, .color = colors[5] },
	};

	uint16_t indices[] = {
		0, 1, 2,  2, 3, 0,
		4, 5, 6,  6, 7, 4,
		8, 9, 10, 10, 11, 8,
		12, 13, 14, 14, 15, 12,
		16, 17, 18, 18, 19, 16,
		20, 21, 22, 22, 23, 20,
	};

	return skr_mesh_create(&skr_vertex_type_pnuc, skr_index_fmt_u16, verts, 24, indices, 36);
}

skr_mesh_t skr_mesh_create_pyramid(float base_size, float height, skr_vec4_t color) {
	float half = base_size * 0.5f;
	float apex_y = height * 0.5f;
	float base_y = -height * 0.5f;

	// Calculate normals for side faces
	float slant_len = sqrtf(half * half + height * height);
	float normal_y = half / slant_len;
	float normal_xz = height / slant_len;

	skr_vertex_pnuc_t verts[17] = {
		// Base (4 vertices)
		{ .position = {-half, base_y,  half, 1.0f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {0.0f, 0.0f}, .color = color },
		{ .position = { half, base_y,  half, 1.0f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {1.0f, 0.0f}, .color = color },
		{ .position = { half, base_y, -half, 1.0f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {1.0f, 1.0f}, .color = color },
		{ .position = {-half, base_y, -half, 1.0f}, .normal = { 0.0f, -1.0f,  0.0f}, .uv = {0.0f, 1.0f}, .color = color },
		// Apex
		{ .position = { 0.0f, apex_y,  0.0f, 1.0f}, .normal = { 0.0f,  1.0f,  0.0f}, .uv = {0.5f, 0.5f}, .color = color },
		// Front face (Z+)
		{ .position = {-half, base_y,  half, 1.0f}, .normal = { 0.0f,  normal_y,  normal_xz}, .uv = {0.0f, 0.0f}, .color = color },
		{ .position = { half, base_y,  half, 1.0f}, .normal = { 0.0f,  normal_y,  normal_xz}, .uv = {1.0f, 0.0f}, .color = color },
		{ .position = { 0.0f, apex_y,  0.0f, 1.0f}, .normal = { 0.0f,  normal_y,  normal_xz}, .uv = {0.5f, 1.0f}, .color = color },
		// Right face (X+)
		{ .position = { half, base_y,  half, 1.0f}, .normal = { normal_xz,  normal_y,  0.0f}, .uv = {0.0f, 0.0f}, .color = color },
		{ .position = { half, base_y, -half, 1.0f}, .normal = { normal_xz,  normal_y,  0.0f}, .uv = {1.0f, 0.0f}, .color = color },
		{ .position = { 0.0f, apex_y,  0.0f, 1.0f}, .normal = { normal_xz,  normal_y,  0.0f}, .uv = {0.5f, 1.0f}, .color = color },
		// Back face (Z-)
		{ .position = { half, base_y, -half, 1.0f}, .normal = { 0.0f,  normal_y, -normal_xz}, .uv = {0.0f, 0.0f}, .color = color },
		{ .position = {-half, base_y, -half, 1.0f}, .normal = { 0.0f,  normal_y, -normal_xz}, .uv = {1.0f, 0.0f}, .color = color },
		{ .position = { 0.0f, apex_y,  0.0f, 1.0f}, .normal = { 0.0f,  normal_y, -normal_xz}, .uv = {0.5f, 1.0f}, .color = color },
		// Left face (X-)
		{ .position = {-half, base_y, -half, 1.0f}, .normal = {-normal_xz,  normal_y,  0.0f}, .uv = {0.0f, 0.0f}, .color = color },
		{ .position = {-half, base_y,  half, 1.0f}, .normal = {-normal_xz,  normal_y,  0.0f}, .uv = {1.0f, 0.0f}, .color = color },
		{ .position = { 0.0f, apex_y,  0.0f, 1.0f}, .normal = {-normal_xz,  normal_y,  0.0f}, .uv = {0.5f, 1.0f}, .color = color },
	};

	uint16_t indices[] = {
		0, 2, 1,  0, 3, 2,     // Base
		5, 6, 7,               // Front
		8, 9, 10,              // Right
		11, 12, 13,            // Back
		14, 15, 16,            // Left
	};

	return skr_mesh_create(&skr_vertex_type_pnuc, skr_index_fmt_u16, verts, 17, indices, 18);
}

skr_mesh_t skr_mesh_create_quad(float width, float height, skr_vec3_t normal, bool double_sided, skr_vec4_t color) {
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

	skr_vertex_pnuc_t* verts = malloc(sizeof(skr_vertex_pnuc_t) * vert_count);
	uint16_t*          inds  = malloc(sizeof(uint16_t) * idx_count);

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

		verts[i] = (skr_vertex_pnuc_t){
			.position = {pos.x, pos.y, pos.z, 1.0f},
			.normal   = normal,
			.uv       = {u, v},
			.color    = color
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

	skr_mesh_t mesh = skr_mesh_create(&skr_vertex_type_pnuc, skr_index_fmt_u16, verts, vert_count, inds, idx_count);
	free(verts);
	free(inds);

	return mesh;
}

skr_mesh_t skr_mesh_create_fullscreen_quad(void) {
	skr_vertex_pnuc_t quad_vertices[] = {
		{ .position = {-1.0f, -1.0f, 0.0f, 1.0f}, .normal = {0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
		{ .position = { 1.0f, -1.0f, 0.0f, 1.0f}, .normal = {0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
		{ .position = { 1.0f,  1.0f, 0.0f, 1.0f}, .normal = {0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
		{ .position = {-1.0f,  1.0f, 0.0f, 1.0f}, .normal = {0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f, 1.0f} },
	};
	uint16_t quad_indices[] = { 0, 1, 2,  2, 3, 0 };
	return skr_mesh_create(&skr_vertex_type_pnuc, skr_index_fmt_u16, quad_vertices, 4, quad_indices, 6);
}

///////////////////////////////////////////////////////////////////////////////
// Texture Generation
///////////////////////////////////////////////////////////////////////////////

skr_tex_t skr_tex_create_checkerboard(int32_t resolution, int32_t square_size, uint32_t color1, uint32_t color2, bool generate_mips) {
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

	skr_tex_t tex = skr_tex_create(
		skr_tex_fmt_rgba32,
		flags,
		skr_sampler_linear_clamp,
		(skr_vec3i_t){resolution, resolution, 1},
		1, 0, pixels
	);

	free(pixels);

	if (generate_mips) {
		skr_tex_generate_mips(&tex, NULL);
	}

	return tex;
}

skr_tex_t skr_tex_create_solid_color(uint32_t color) {
	return skr_tex_create(
		skr_tex_fmt_rgba32,
		skr_tex_flags_readable,
		skr_sampler_linear_clamp,
		(skr_vec3i_t){1, 1, 1},
		1, 1, &color
	);
}

///////////////////////////////////////////////////////////////////////////////
// Image Loading
///////////////////////////////////////////////////////////////////////////////

unsigned char* skr_image_load(const char* filename, int32_t* opt_out_width, int32_t* opt_out_height, int32_t* opt_out_channels, int32_t force_channels) {
	void*  file_data = NULL;
	size_t file_size = 0;

	if (!app_read_file(filename, &file_data, &file_size)) {
		return NULL;
	}

	unsigned char* pixels = skr_image_load_from_memory(file_data, file_size, opt_out_width, opt_out_height, opt_out_channels, force_channels);
	free(file_data);

	return pixels;
}

unsigned char* skr_image_load_from_memory(const void* data, size_t size, int32_t* opt_out_width, int32_t* opt_out_height, int32_t* opt_out_channels, int32_t force_channels) {
	int width, height, channels;
	unsigned char* pixels = stbi_load_from_memory((const unsigned char*)data, (int)size, &width, &height, &channels, force_channels);

	if (pixels) {
		if (opt_out_width)    *opt_out_width    = width;
		if (opt_out_height)   *opt_out_height   = height;
		if (opt_out_channels) *opt_out_channels = channels;
	}

	return pixels;
}

void skr_image_free(unsigned char* pixels) {
	stbi_image_free(pixels);
}

///////////////////////////////////////////////////////////////////////////////
// Utility Functions
///////////////////////////////////////////////////////////////////////////////

float skr_hash_f(int32_t position, uint32_t seed) {
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
