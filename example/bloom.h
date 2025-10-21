#pragma once

#include "../include/sk_renderer.h"

typedef struct bloom_t {
	skr_tex_t       bloom_chain[7];
	skr_tex_t       bloom_upsample[7];
	skr_compute_t   bloom_downsample_comp[7];
	skr_compute_t   bloom_upsample_comp[7];
	skr_material_t  bloom_composite_mat;
	skr_mesh_t      fullscreen_quad;
	skr_buffer_t    bloom_params_buffers[7];
	skr_buffer_t    composite_params_buffer;
	skr_shader_t    bloom_downsample_shader;
	skr_shader_t    bloom_upsample_shader;
	skr_shader_t    bloom_composite_shader;
	skr_vert_type_t vertex_type;
	int32_t         bloom_mips;
	int32_t         width;
	int32_t         height;
} bloom_t;

void bloom_create  (int32_t width, int32_t height, int32_t mip_count);
void bloom_apply   (skr_tex_t* scene_color, skr_tex_t* target, float bloom_strength, float radius, float intensity);
void bloom_resize  (int32_t width, int32_t height);
void bloom_destroy (void);
