#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "sksc_file.h"

///////////////////////////////////////////////////////////////////////////////

typedef struct skr_vec2_t {
	float x;
	float y;
} skr_vec2_t;

typedef struct skr_vec3_t {
	float x;
	float y;
	float z;
} skr_vec3_t;

typedef struct skr_vec4_t {
	float x;
	float y;
	float z;
	float w;
} skr_vec4_t;

typedef struct skr_vec2i_t {
	int32_t x;
	int32_t y;
} skr_vec2i_t;

typedef struct skr_vec3i_t {
	int32_t x;
	int32_t y;
	int32_t z;
} skr_vec3i_t;

typedef struct skr_vec4i_t {
	int32_t x;
	int32_t y;
	int32_t z;
	int32_t w;
} skr_vec4i_t;

typedef struct skr_quat_t {
	float x;
	float y;
	float z;
	float w;
} skr_quat_t;

typedef struct skr_matrix {
    // row-major
	float m[16];
} skr_matrix_t;

typedef struct skr_rect_t {
	float x;
	float y;
	float w;
	float h;
} skr_rect_t;

typedef struct skr_recti_t {
	int32_t x;
	int32_t y;
	int32_t w;
	int32_t h;
} skr_recti_t;

///////////////////////////////////////////////////////////////////////////////

typedef enum skr_buffer_type_ {
	skr_buffer_type_vertex,
	skr_buffer_type_index,
	skr_buffer_type_constant,
	skr_buffer_type_storage,   // For storage buffers (SSBOs) - compute, instance data, etc.
} skr_buffer_type_;

typedef enum skr_use_ {
	skr_use_static        = 1 << 1,
	skr_use_dynamic       = 1 << 2,
	skr_use_compute_read  = 1 << 3,
	skr_use_compute_write = 1 << 4,
	skr_use_compute_readwrite = skr_use_compute_read | skr_use_compute_write
} skr_use_;

typedef enum skr_tex_fmt_ {
	skr_tex_fmt_none = 0,
	skr_tex_fmt_rgba32 = 1,
	skr_tex_fmt_rgba32_linear = 2,
	skr_tex_fmt_bgra32 = 3,
	skr_tex_fmt_bgra32_linear = 4,
	skr_tex_fmt_rg11b10 = 5,
	skr_tex_fmt_rgb10a2 = 6,
	skr_tex_fmt_rgba64u = 7,
	skr_tex_fmt_rgba64s = 8,
	skr_tex_fmt_rgba64f = 9,
	skr_tex_fmt_rgba128 = 10,
	skr_tex_fmt_r8 = 11,
	skr_tex_fmt_r16u = 12,
	skr_tex_fmt_r16s = 13,
	skr_tex_fmt_r16f = 14,
	skr_tex_fmt_r32 = 15,
	skr_tex_fmt_depth24s8 = 16,
	skr_tex_fmt_depth32 = 17,
	skr_tex_fmt_depth16 = 18,
	skr_tex_fmt_r8g8 = 19,
	skr_tex_fmt_rgb9e5 = 20,

	skr_tex_fmt_depth32s8,
	skr_tex_fmt_depth16s8,


	skr_tex_fmt_bc1_rgb_srgb,
	skr_tex_fmt_bc1_rgb,
	skr_tex_fmt_bc3_rgba_srgb,
	skr_tex_fmt_bc3_rgba,
	skr_tex_fmt_bc4_r,
	skr_tex_fmt_bc5_rg,
	skr_tex_fmt_bc7_rgba_srgb,
	skr_tex_fmt_bc7_rgba,

	skr_tex_fmt_etc1_rgb,
	skr_tex_fmt_etc2_rgba_srgb,
	skr_tex_fmt_etc2_rgba,
	skr_tex_fmt_etc2_r11,
	skr_tex_fmt_etc2_rg11,
	skr_tex_fmt_pvrtc1_rgb_srgb,
	skr_tex_fmt_pvrtc1_rgb,
	skr_tex_fmt_pvrtc1_rgba_srgb,
	skr_tex_fmt_pvrtc1_rgba,
	skr_tex_fmt_pvrtc2_rgba_srgb,
	skr_tex_fmt_pvrtc2_rgba,
	skr_tex_fmt_astc4x4_rgba_srgb,
	skr_tex_fmt_astc4x4_rgba,
	skr_tex_fmt_atc_rgb,
	skr_tex_fmt_atc_rgba,
} skr_tex_fmt_;

typedef enum skr_tex_flags_ {
	skr_tex_flags_readable  = 1 << 0,
	skr_tex_flags_writeable = 1 << 1,
	skr_tex_flags_dynamic   = 1 << 2,
	skr_tex_flags_gen_mips  = 1 << 3,
	skr_tex_flags_array     = 1 << 4,
	skr_tex_flags_3d        = 1 << 5,
	skr_tex_flags_in_tile_msaa = 1 << 6,
	skr_tex_flags_compute   = 1 << 7,  // For compute shader RWTexture (storage image)
	skr_tex_flags_cubemap   = 1 << 8,  // Cubemap texture (requires 6 array layers)
} skr_tex_flags_;

typedef enum skr_tex_sample_ {
	skr_tex_sample_linear = 0,
	skr_tex_sample_point,
	skr_tex_sample_anisotropic
} skr_tex_sample_;

typedef enum skr_tex_address_ {
	skr_tex_address_wrap = 0,
	skr_tex_address_clamp,
	skr_tex_address_mirror,
} skr_tex_address_;

typedef enum skr_mip_filter_ {
	skr_mip_filter_default = 0,     // vkCmdBlitImage with linear filtering
	skr_mip_filter_alpha_weighted,  // Compute shader with alpha-weighted averaging
	skr_mip_filter_sdf,             // Compute shader optimized for SDF data
} skr_mip_filter_;

typedef enum skr_compare_ {
	skr_compare_none = 0,
	skr_compare_less,
	skr_compare_less_or_eq,
	skr_compare_greater,
	skr_compare_greater_or_eq,
	skr_compare_equal,
	skr_compare_not_equal,
	skr_compare_always,
	skr_compare_never,
} skr_compare_;

typedef enum skr_blend_factor_ {
	skr_blend_zero = 0,
	skr_blend_one,
	skr_blend_src_color,
	skr_blend_one_minus_src_color,
	skr_blend_dst_color,
	skr_blend_one_minus_dst_color,
	skr_blend_src_alpha,
	skr_blend_one_minus_src_alpha,
	skr_blend_dst_alpha,
	skr_blend_one_minus_dst_alpha,
	skr_blend_constant_color,
	skr_blend_one_minus_constant_color,
	skr_blend_constant_alpha,
	skr_blend_one_minus_constant_alpha,
	skr_blend_src_alpha_saturate,
	skr_blend_src1_color,
	skr_blend_one_minus_src1_color,
	skr_blend_src1_alpha,
	skr_blend_one_minus_src1_alpha,
} skr_blend_factor_;

typedef enum skr_blend_op_ {
	skr_blend_op_add = 0,
	skr_blend_op_subtract,
	skr_blend_op_reverse_subtract,
	skr_blend_op_min,
	skr_blend_op_max,
} skr_blend_op_;

typedef enum skr_stencil_op_ {
	skr_stencil_op_keep = 0,
	skr_stencil_op_zero,
	skr_stencil_op_replace,
	skr_stencil_op_increment_clamp,
	skr_stencil_op_decrement_clamp,
	skr_stencil_op_invert,
	skr_stencil_op_increment_wrap,
	skr_stencil_op_decrement_wrap,
} skr_stencil_op_;

typedef enum skr_write_ {
	skr_write_r       = 1 << 0,
	skr_write_g       = 1 << 1,
	skr_write_b       = 1 << 2,
	skr_write_a       = 1 << 3,
	skr_write_depth   = 1 << 4,
	skr_write_stencil = 1 << 5,
	skr_write_rgba    = skr_write_r | skr_write_g | skr_write_b | skr_write_a,
	skr_write_default = skr_write_r | skr_write_g | skr_write_b | skr_write_a | skr_write_depth
} skr_write_;

typedef enum skr_cull_ {
	skr_cull_back = 0,
	skr_cull_front,
	skr_cull_none,
} skr_cull_;

typedef enum skr_index_fmt_ {
	skr_index_fmt_u8,
	skr_index_fmt_u16,
	skr_index_fmt_u32,
} skr_index_fmt_;

typedef enum skr_log_ {
	skr_log_info,
	skr_log_warning,
	skr_log_critical,
} skr_log_;

typedef enum {
	skr_register_default,
	skr_register_vertex,
	skr_register_index,
	skr_register_constant,
	skr_register_texture,
	skr_register_read_buffer,
	skr_register_readwrite,
	skr_register_readwrite_tex,
} skr_register_;

typedef enum skr_clear_ {
	skr_clear_none    = 0,
	skr_clear_color   = 1 << 0,
	skr_clear_depth   = 1 << 1,
	skr_clear_stencil = 1 << 2,
	skr_clear_all     = skr_clear_color | skr_clear_depth | skr_clear_stencil,
} skr_clear_;

typedef struct skr_tex_sampler_t {
	skr_tex_sample_      sample;
	skr_tex_address_     address;
	skr_compare_         sample_compare;
	int32_t             anisotropy;
} skr_tex_sampler_t;

typedef struct skr_blend_state_t {
	skr_blend_factor_ src_color_factor;
	skr_blend_factor_ dst_color_factor;
	skr_blend_op_     color_op;
	skr_blend_factor_ src_alpha_factor;
	skr_blend_factor_ dst_alpha_factor;
	skr_blend_op_     alpha_op;
} skr_blend_state_t;

// Common blend state presets
static const skr_blend_state_t skr_blend_off = {0};

static const skr_blend_state_t skr_blend_alpha = {
	.src_color_factor = skr_blend_src_alpha,
	.dst_color_factor = skr_blend_one_minus_src_alpha,
	.color_op         = skr_blend_op_add,
	.src_alpha_factor = skr_blend_one,
	.dst_alpha_factor = skr_blend_one_minus_src_alpha,
	.alpha_op         = skr_blend_op_add,
};

static const skr_blend_state_t skr_blend_additive = {
	.src_color_factor = skr_blend_src_alpha,
	.dst_color_factor = skr_blend_one,
	.color_op         = skr_blend_op_add,
	.src_alpha_factor = skr_blend_one,
	.dst_alpha_factor = skr_blend_one,
	.alpha_op         = skr_blend_op_add,
};

static const skr_blend_state_t skr_blend_premultiplied = {
	.src_color_factor = skr_blend_one,
	.dst_color_factor = skr_blend_one_minus_src_alpha,
	.color_op         = skr_blend_op_add,
	.src_alpha_factor = skr_blend_one,
	.dst_alpha_factor = skr_blend_one_minus_src_alpha,
	.alpha_op         = skr_blend_op_add,
};

typedef struct skr_stencil_state_t {
	skr_compare_    compare;
	skr_stencil_op_ fail_op;
	skr_stencil_op_ pass_op;
	skr_stencil_op_ depth_fail_op;
	uint32_t        compare_mask;
	uint32_t        write_mask;
	uint32_t        reference;
} skr_stencil_state_t;

// This may need to be per-backend?
typedef struct skr_settings_t {
	const char*  app_name;
	int32_t      app_version;
	bool         enable_validation;
	const char** required_extensions;
	uint32_t     required_extension_count;
} skr_settings_t;

typedef struct skr_shader_t skr_shader_t;

typedef struct skr_material_info_t {
	const skr_shader_t*  shader;
	skr_cull_            cull;
	skr_write_           write_mask;
	skr_compare_         depth_test;
	skr_blend_state_t    blend_state;
	bool                 alpha_to_coverage;
	skr_stencil_state_t  stencil_front;
	skr_stencil_state_t  stencil_back;
	int32_t              queue_offset;  // Render queue offset for sorting (lower draws first)
} skr_material_info_t;

// While this project is primarily Vulkan, the option to add backends in the
// future would be nice. WebGPU or D3D12 could be targets. However, we don't
// want to introduce pointer indirection to core graphics assets! We risk a bit
// of API bleed by letting each backend define its own structs, but gain the
// flexibility to manage the memory our structures use.
#define SKR_VK
#ifdef SKR_VK
#include "../vk/skr_vulkan.h"
VkInstance        skr_get_vk_instance              (void);
VkDevice          skr_get_vk_device                (void);
#endif

///////////////////////////////////////////////////////////////////////////////

bool              skr_init                         (skr_settings_t settings);
void              skr_shutdown                     (void);
void              skr_callback_log                 (void (*callback)(skr_log_ level, const char* text));
void              skr_log                          (skr_log_ level, const char* text);
void              skr_logf                         (skr_log_ level, const char* text, ...);

skr_buffer_t      skr_buffer_create                (const void *data, uint32_t size_count, uint32_t size_stride, skr_buffer_type_ type, skr_use_ use);
void              skr_buffer_destroy               (      skr_buffer_t *buffer);
bool              skr_buffer_is_valid              (const skr_buffer_t *buffer);
void              skr_buffer_set                   (      skr_buffer_t *buffer, const void *data, uint32_t size_bytes);
void              skr_buffer_get                   (const skr_buffer_t *buffer, void *ref_buffer, uint32_t buffer_size);
uint32_t          skr_buffer_get_size              (const skr_buffer_t *buffer);
void              skr_buffer_set_name              (      skr_buffer_t *buffer, const char* name);

skr_vert_type_t   skr_vert_type_create             (const skr_vert_component_t* items, int32_t item_count);
bool              skr_vert_type_is_valid           (const skr_vert_component_t* type);
void              skr_vert_type_destroy            (      skr_vert_type_t* type);

skr_mesh_t        skr_mesh_create                  (const skr_vert_type_t* vert_type, skr_index_fmt_ ind_type, const void* vert_data, uint32_t vert_count, const void* opt_ind_data, uint32_t ind_count);
bool              skr_mesh_is_valid                (const skr_mesh_t* mesh);
void              skr_mesh_destroy                 (      skr_mesh_t* mesh);
uint32_t          skr_mesh_get_vert_count          (const skr_mesh_t* mesh);
uint32_t          skr_mesh_get_ind_count           (const skr_mesh_t* mesh);
void              skr_mesh_set_name                (      skr_mesh_t* mesh, const char* name);

skr_tex_t         skr_tex_create                   (skr_tex_fmt_ format, skr_tex_flags_ flags, skr_tex_sampler_t sampler, skr_vec3i_t size, int32_t multisample, int32_t mip_count, const void* opt_tex_data);
bool              skr_tex_is_valid                 (const skr_tex_t* tex);
void              skr_tex_destroy                  (      skr_tex_t* tex);
skr_tex_t         skr_tex_duplicate                (const skr_tex_t* tex, skr_tex_fmt_ to_format, skr_tex_flags_ to_flags);
void*             skr_tex_get_data                 (const skr_tex_t* tex, int32_t array_idx, int32_t mip_level);
skr_vec3i_t       skr_tex_get_size                 (const skr_tex_t* tex);
skr_tex_fmt_      skr_tex_get_format               (const skr_tex_t* tex);
skr_tex_flags_    skr_tex_get_flags                (const skr_tex_t* tex);
int32_t           skr_tex_get_multisample          (const skr_tex_t* tex);
void              skr_tex_set_sampler              (      skr_tex_t* tex, skr_tex_sampler_t sampler);
skr_tex_sampler_t skr_tex_get_sampler              (const skr_tex_t* tex);
bool              skr_tex_fmt_is_supported         (skr_tex_fmt_ format);
void              skr_tex_generate_mips            (      skr_tex_t* tex, const skr_shader_t* opt_compute_shader);
void              skr_tex_set_name                 (      skr_tex_t* tex, const char* name);

skr_surface_t     skr_surface_create               (void* vk_surface_khr);
void              skr_surface_destroy              (      skr_surface_t* surface);
void              skr_surface_resize               (      skr_surface_t* surface);
skr_tex_t*        skr_surface_next_tex             (      skr_surface_t* surface);
void              skr_surface_present              (      skr_surface_t* surface);
skr_vec2i_t       skr_surface_get_size             (const skr_surface_t* surface);

skr_shader_t      skr_shader_create                (const void *shader_data, size_t data_size);
bool              skr_shader_is_valid              (const skr_shader_t* shader);
void              skr_shader_destroy               (      skr_shader_t* shader);
skr_bind_t        skr_shader_get_bind              (const skr_shader_t* shader, const char* bind_name);
void              skr_shader_set_name              (      skr_shader_t* shader, const char* name);

skr_compute_t     skr_compute_create               (const skr_shader_t* shader);
bool              skr_compute_is_valid             (const skr_compute_t* shader);
void              skr_compute_destroy              (      skr_compute_t* shader);
skr_bind_t        skr_compute_get_bind             (const skr_compute_t* shader, const char* bind_name);
void              skr_compute_execute              (      skr_compute_t* shader, uint32_t x, uint32_t y, uint32_t z);
void              skr_compute_execute_indirect     (      skr_compute_t* shader, skr_buffer_t* indirect_args);
void              skr_compute_set_tex              (      skr_compute_t* shader, int32_t bind, skr_tex_t*    texture);
void              skr_compute_set_buffer           (      skr_compute_t* shader, int32_t bind, skr_buffer_t* buffer);

skr_material_t    skr_material_create              (skr_material_info_t info);
bool              skr_material_is_valid            (const skr_material_t* material);
void              skr_material_set_tex             (      skr_material_t* material, int32_t bind, skr_tex_t*    texture);
void              skr_material_set_buffer          (      skr_material_t* material, int32_t bind, skr_buffer_t* buffer);
void              skr_material_destroy             (      skr_material_t* material);

skr_render_list_t skr_render_list_create           ();
void              skr_render_list_destroy          (skr_render_list_t* list);
void              skr_render_list_clear            (skr_render_list_t* list);
void              skr_render_list_add              (skr_render_list_t* list, skr_mesh_t* mesh, skr_material_t* material, const void* opt_instance_data, uint32_t instance_data_size, uint32_t instance_count);

void              skr_renderer_frame_begin         ();
void              skr_renderer_frame_end           ();
void              skr_renderer_begin_pass          (skr_tex_t* color, skr_tex_t* depth, skr_tex_t* opt_resolve, skr_clear_ clear, skr_vec4_t clear_color, float clear_depth, uint32_t clear_stencil);
void              skr_renderer_end_pass            ();
void              skr_renderer_set_global_constants(int32_t bind, const skr_buffer_t* buffer);
void              skr_renderer_set_global_texture  (int32_t bind, const skr_tex_t* tex);
void              skr_renderer_set_viewport        (skr_rect_t viewport);
void              skr_renderer_set_scissor         (skr_recti_t scissor);
void              skr_renderer_blit                (skr_material_t* material, skr_tex_t* to, skr_recti_t bounds_px);

void              skr_renderer_draw                (skr_render_list_t* list, const void* system_data, size_t system_data_size, int32_t instance_multiplier);
float             skr_renderer_get_gpu_time_ms     ();

#ifdef __cplusplus
}
#endif