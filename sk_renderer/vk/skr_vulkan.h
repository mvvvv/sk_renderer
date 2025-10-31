// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#pragma once

#include <volk.h>

#define SKR_MAX_FRAMES_IN_FLIGHT 3

typedef struct skr_buffer_t {
	VkBuffer            buffer;
	VkDeviceMemory      memory;
	uint32_t            size;
	skr_buffer_type_    type;
	skr_use_            use;
	void*               mapped; // For dynamic buffers
} skr_buffer_t;

typedef struct skr_vert_type_t {
	VkVertexInputAttributeDescription* attributes;
	VkVertexInputBindingDescription    binding;
	skr_vert_component_t*              components;
	uint32_t                           component_count;
	int32_t                            pipeline_idx; // Cached pipeline vertex format index
} skr_vert_type_t;

typedef struct skr_mesh_t {
	skr_buffer_t           vertex_buffer;
	skr_buffer_t           index_buffer;
	const skr_vert_type_t* vert_type;
	skr_index_fmt_         ind_format;
	VkIndexType            ind_format_vk;
	uint32_t               vert_count;
	uint32_t               ind_count;
} skr_mesh_t;

typedef struct skr_tex_t {
	VkImage                image;
	VkDeviceMemory         memory;
	VkImageView            view;
	VkFramebuffer          framebuffer;      // Cached framebuffer (color only, no depth)
	VkFramebuffer          framebuffer_depth; // Cached framebuffer (color + depth, if last used with depth)
	VkRenderPass           framebuffer_pass; // Render pass the framebuffer was created for
	VkSampler              sampler;          // Vulkan sampler handle
	skr_tex_sampler_t      sampler_settings; // Sampler settings
	skr_vec3i_t            size;
	skr_tex_fmt_           format;
	skr_tex_flags_         flags;
	VkSampleCountFlagBits  samples;          // Sample count for MSAA (VK_SAMPLE_COUNT_1_BIT, 2, 4, 8, etc.)
	uint32_t               mip_levels;       // Number of mip levels
	uint32_t               layer_count;      // Number of array layers (1 for regular, N for arrays, 6 for cubemaps)
	VkImageAspectFlags     aspect_mask;      // Depth bit for depth textures, color bit for color textures

	// Automatic layout transition tracking
	VkImageLayout          current_layout;       // Current image layout (tracked automatically)
	uint32_t               current_queue_family; // Current queue family owner
	bool                   first_use;            // True until first transition (allows UNDEFINED optimization)
	bool                   is_transient_discard; // True for non-readable depth/MSAA (always use UNDEFINED)
} skr_tex_t;

typedef struct skr_surface_t {
	VkSurfaceKHR   surface;
	VkSwapchainKHR swapchain;
	skr_tex_t*     images;
	uint32_t       image_count;
	uint32_t       current_image;
	uint32_t       frame_idx;
	VkFence        fence_frame      [SKR_MAX_FRAMES_IN_FLIGHT];
	VkSemaphore    semaphore_acquire[SKR_MAX_FRAMES_IN_FLIGHT];
	VkSemaphore*   semaphore_submit;
	skr_vec2i_t    size;
} skr_surface_t;

typedef struct skr_shader_stage_t {
	VkShaderModule shader;
	skr_stage_     type;
} skr_shader_stage_t;

typedef struct skr_shader_t {
	sksc_shader_meta_t*  meta;
	skr_shader_stage_t  vertex_stage;
	skr_shader_stage_t  pixel_stage;
	skr_shader_stage_t  compute_stage;
} skr_shader_t;

typedef struct  {
	union {
		skr_tex_t*    texture;
		skr_buffer_t* buffer;
	};
	skr_bind_t bind;
} skr_material_bind_t;

typedef struct skr_material_t {
	int32_t                pipeline_material_idx; // Index into pipeline cache
	skr_material_info_t    info;                  // Material state (used as pipeline key)

	skr_material_bind_t*   binds;
	uint32_t               bind_count;
	// Material parameters
	void*                  param_buffer;          // CPU-side parameter data
	uint32_t               param_buffer_size;     // Size of parameter buffer in bytes
	bool                   param_buffer_dirty;    // True if parameters changed since last add to render list

	bool                   has_system_buffer;
} skr_material_t;

typedef struct skr_compute_t {
	const skr_shader_t*   shader;  // Reference to shader (not owned)
	VkPipelineLayout      layout;
	VkDescriptorSetLayout descriptor_layout;
	VkPipeline            pipeline;
	
	skr_material_bind_t*  binds;
	uint32_t              bind_count;
} skr_compute_t;

typedef struct skr_render_item_t {
	skr_mesh_t*     mesh;
	skr_material_t* material;
	uint32_t        instance_offset;      // Offset into instance_data (bytes)
	uint32_t        instance_data_size;   // Size per instance (bytes)
	uint32_t        instance_count;       // Number of instances
} skr_render_item_t;

typedef struct skr_render_list_t {
	skr_render_item_t* items;
	uint32_t           count;
	uint32_t           capacity;
	uint8_t*           instance_data;
	uint32_t           instance_data_used;
	uint32_t           instance_data_capacity;
	uint8_t*           instance_data_sorted;          // Reordered instance data after sort
	uint32_t           instance_data_sorted_capacity;
	uint8_t*           material_data;
	uint32_t           material_data_used;
	uint32_t           material_data_capacity;
	// GPU buffers (uploaded once per frame)
	skr_buffer_t       instance_buffer;
	bool               instance_buffer_valid;
	skr_buffer_t       material_param_buffer;
	bool               material_param_buffer_valid;
	skr_buffer_t       system_buffer;
	bool               system_buffer_valid;
	bool               needs_sort;  // Dirty flag for sorting
} skr_render_list_t;

typedef struct skr_future_t {
	void*    slot;          // Pointer to _skr_cmd_ring_slot_t
	uint64_t generation;    // Generation counter to detect fence reuse (must match slot's generation)
} skr_future_t;