// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#pragma once

#include "sk_renderer.h"
#include "skr_vulkan.h"

#include <volk.h>
#include <pthread.h>

///////////////////////////////////////////////////////////////////////////////
// Internal state
///////////////////////////////////////////////////////////////////////////////

typedef struct {
	VkFormat              color_format;
	VkFormat              depth_format;
	VkFormat              resolve_format;
	VkSampleCountFlagBits samples;
	VkAttachmentStoreOp   depth_store_op;   // How to store depth (STORE or DONT_CARE)
	VkAttachmentLoadOp    color_load_op;    // How to load color (LOAD, CLEAR, or DONT_CARE)
} skr_pipeline_renderpass_key_t;

#define SKR_MAX_FRAMES_IN_FLIGHT 3
#define skr_MAX_COMMAND_RING    8   // Number of command buffers per thread
#define skr_MAX_THREAD_POOLS    16  // Maximum concurrent threads

#define SKR_BIND_MATERIAL 0
#define SKR_BIND_SYSTEM 1
#define SKR_BIND_INSTANCE 2

#define SKR_BIND_SHIFT_BUFFER  0
#define SKR_BIND_SHIFT_TEXTURE 100
#define SKR_BIND_SHIFT_UAV     200

// Deferred destruction system
typedef struct skr_destroy_list_t {
	void*    items;
	uint32_t count;
	uint32_t capacity;
} skr_destroy_list_t;

typedef struct {
	VkCommandBuffer    cmd;
	VkFence            fence;
	skr_destroy_list_t destroy_list;
	bool               alive;
} _skr_command_ring_slot_t;

// Command context returned from command begin/acquire
typedef struct {
	VkCommandBuffer     cmd;
	skr_destroy_list_t* destroy_list;
} _skr_command_context_t;

typedef struct {
	VkCommandPool              cmd_pool;
	_skr_command_ring_slot_t*  active_cmd; 
	_skr_command_ring_slot_t   cmd_ring[skr_MAX_COMMAND_RING];
	uint32_t                   cmd_ring_index;
	uint64_t                   thread_id;
	int32_t                    ref_count;
	bool                       alive;
} _skr_vk_thread_t;

typedef struct {
	VkDescriptorSet sets[64];      // Pool of descriptor sets
	uint32_t        next_free;     // Next free index
	uint32_t        flight_marker; // Which flight owns these sets (for recycling)
} _skr_compute_descriptor_pool_t;

typedef struct {
	VkInstance               instance;
	VkPhysicalDevice         physical_device;
	VkDevice                 device;
	VkQueue                  graphics_queue;
	VkQueue                  present_queue;
	uint32_t                 graphics_queue_family;
	uint32_t                 present_queue_family;
	VkCommandPool            command_pool;
	VkCommandBuffer          command_buffers[SKR_MAX_FRAMES_IN_FLIGHT];
	VkFence                  frame_fences[SKR_MAX_FRAMES_IN_FLIGHT];
	VkPipelineCache          pipeline_cache;
	VkDescriptorPool         descriptor_pool;
	VkDebugUtilsMessengerEXT debug_messenger;
	bool                     validation_enabled;
	bool                     initialized;
	bool                     in_frame;  // True when between frame_begin and frame_end
	pthread_t                main_thread_id;  // Thread that calls skr_init
	uint32_t                 frame;
	uint32_t                 flight_idx;

	// GPU timing (single query pool, 2 queries per frame)
	VkQueryPool              timestamp_pool;
	float                    timestamp_period;  // ns per tick
	uint64_t                 frame_timestamps[SKR_MAX_FRAMES_IN_FLIGHT][2];  // [frame][start/end]
	bool                     timestamps_valid[SKR_MAX_FRAMES_IN_FLIGHT];

	// Current render pass (for pipeline lookup)
	int32_t                  current_renderpass_idx;
	skr_tex_t*               current_color_texture;  // Track color texture for layout transitions
	skr_tex_t*               current_depth_texture;  // Track depth texture for layout transitions

	// Global bindings (merged with material bindings at draw time)
	skr_buffer_t*            global_buffers[16];
	skr_tex_t*               global_textures[16];

	// Deferred texture transition tracking (to avoid in-renderpass barriers)
	skr_tex_t*               pending_transitions[64];
	uint8_t                  pending_transition_types[64];  // 0=shader_read, 1=storage
	uint32_t                 pending_transition_count;

	// Command system
	VkQueue                  transfer_queue;
	uint32_t                 transfer_queue_family;
	bool                     has_dedicated_transfer;
	_skr_vk_thread_t*        thread_pools[skr_MAX_THREAD_POOLS];
	uint32_t                 thread_pool_ct;
	pthread_mutex_t          thread_pool_mutex;

	// Default assets
	skr_tex_t                default_tex_white;
	skr_tex_t                default_tex_black;
	skr_tex_t                default_tex_gray;

	// Deferred destruction
	skr_destroy_list_t       destroy_list;
} _skr_vk_t;

extern _skr_vk_t _skr_vk;

///////////////////////////////////////////////////////////////////////////////
// Internal helpers
///////////////////////////////////////////////////////////////////////////////

VkFramebuffer   _skr_create_framebuffer(VkRenderPass render_pass, skr_tex_t* color, skr_tex_t* depth, skr_tex_t* opt_resolve);
VkSampler       _skr_sampler_create_vk (skr_tex_sampler_t settings);
VkDescriptorSetLayout _skr_shader_make_layout(const sksc_shader_meta_t* meta, skr_stage_ stage_mask);

// Format helpers
bool            _skr_format_has_stencil(VkFormat format);

// Material descriptor caching
void            _skr_material_add_writes(const skr_material_bind_t* binds, uint32_t bind_ct, const int32_t* ignore_slots, int32_t ignore_ct, VkWriteDescriptorSet* ref_writes, uint32_t write_max, VkDescriptorBufferInfo* ref_buffer_infos, uint32_t buffer_max, VkDescriptorImageInfo* ref_image_infos, uint32_t image_max, uint32_t* ref_write_ct, uint32_t* ref_buffer_ct, uint32_t* ref_image_ct);

// Render list sorting
void            _skr_render_list_sort(skr_render_list_t* list);

// Debug
void            _skr_set_debug_name          (VkObjectType type, uint64_t handle, const char* name);
void            _skr_append_vertex_format    (char* str, size_t str_size, const skr_vert_component_t* components, uint32_t component_count);
void            _skr_append_material_config  (char* str, size_t str_size, const skr_material_info_t* mat_info);
void            _skr_append_renderpass_config(char* str, size_t str_size, const skr_pipeline_renderpass_key_t* rp_key);
void            _skr_log_descriptor_writes   (const VkWriteDescriptorSet* writes, const VkDescriptorBufferInfo* buffer_infos, const VkDescriptorImageInfo* image_infos, uint32_t write_ct, uint32_t buffer_ct, uint32_t image_ct);

// Automatic layout transition system
void            _skr_tex_transition                (VkCommandBuffer cmd, skr_tex_t* tex, VkImageLayout new_layout, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access);
void            _skr_tex_transition_for_shader_read(VkCommandBuffer cmd, skr_tex_t* tex, VkPipelineStageFlags dst_stage);
void            _skr_tex_transition_for_storage    (VkCommandBuffer cmd, skr_tex_t* tex);
void            _skr_tex_transition_queue_family   (VkCommandBuffer cmd, skr_tex_t* tex, uint32_t src_queue_family, uint32_t dst_queue_family, VkImageLayout layout);
void            _skr_tex_transition_notify_layout  (skr_tex_t* tex, VkImageLayout new_layout);  // Called after render pass implicit transitions
bool            _skr_tex_needs_transition          (const skr_tex_t* tex, uint8_t type); // Check if texture needs transition for given type (0=shader_read, 1=storage)
void            _skr_tex_transition_enqueue        (skr_tex_t* tex, uint8_t type); // Deferred texture transition queue (to avoid in-renderpass barriers) type: 0=shader_read, 1=storage

// Command buffer management
bool                    _skr_command_init          (void);
void                    _skr_command_shutdown      (void);
_skr_command_context_t  _skr_command_begin         (void);
bool                    _skr_command_try_get_active(_skr_command_context_t* out_ctx);
VkCommandBuffer         _skr_command_end           (void);  // Ends and returns command buffer (caller must submit)
void                    _skr_command_end_submit    (const VkSemaphore* opt_wait_semaphore, const VkSemaphore* opt_signal_semaphore, VkFence* out_opt_fence);  // Ends and submits with optional semaphores/fence
_skr_command_context_t  _skr_command_acquire       (void);
void                    _skr_command_release       (VkCommandBuffer buffer);

// Deferred destruction API
skr_destroy_list_t _skr_destroy_list_create     (void);
void               _skr_destroy_list_free       (skr_destroy_list_t* list);
void               _skr_destroy_list_execute    (skr_destroy_list_t* list);
void               _skr_destroy_list_clear      (skr_destroy_list_t* list);

// Add functions for each Vulkan resource type
void _skr_destroy_list_add_buffer               (skr_destroy_list_t* list, VkBuffer                 handle);
void _skr_destroy_list_add_image                (skr_destroy_list_t* list, VkImage                  handle);
void _skr_destroy_list_add_image_view           (skr_destroy_list_t* list, VkImageView              handle);
void _skr_destroy_list_add_sampler              (skr_destroy_list_t* list, VkSampler                handle);
void _skr_destroy_list_add_framebuffer          (skr_destroy_list_t* list, VkFramebuffer            handle);
void _skr_destroy_list_add_render_pass          (skr_destroy_list_t* list, VkRenderPass             handle);
void _skr_destroy_list_add_pipeline             (skr_destroy_list_t* list, VkPipeline               handle);
void _skr_destroy_list_add_pipeline_layout      (skr_destroy_list_t* list, VkPipelineLayout         handle);
void _skr_destroy_list_add_pipeline_cache       (skr_destroy_list_t* list, VkPipelineCache          handle);
void _skr_destroy_list_add_descriptor_set_layout(skr_destroy_list_t* list, VkDescriptorSetLayout    handle);
void _skr_destroy_list_add_descriptor_pool      (skr_destroy_list_t* list, VkDescriptorPool         handle);
void _skr_destroy_list_add_shader_module        (skr_destroy_list_t* list, VkShaderModule           handle);
void _skr_destroy_list_add_command_pool         (skr_destroy_list_t* list, VkCommandPool            handle);
void _skr_destroy_list_add_fence                (skr_destroy_list_t* list, VkFence                  handle);
void _skr_destroy_list_add_semaphore            (skr_destroy_list_t* list, VkSemaphore              handle);
void _skr_destroy_list_add_query_pool           (skr_destroy_list_t* list, VkQueryPool              handle);
void _skr_destroy_list_add_swapchain            (skr_destroy_list_t* list, VkSwapchainKHR           handle);
void _skr_destroy_list_add_surface              (skr_destroy_list_t* list, VkSurfaceKHR             handle);
void _skr_destroy_list_add_debug_messenger      (skr_destroy_list_t* list, VkDebugUtilsMessengerEXT handle);
void _skr_destroy_list_add_memory               (skr_destroy_list_t* list, VkDeviceMemory           handle);
