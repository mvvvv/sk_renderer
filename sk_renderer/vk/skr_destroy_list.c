// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "_sk_renderer.h"
#include "../skr_log.h"

#include <stdlib.h>

///////////////////////////////////////////////////////////////////////////////
// Destroy list implementation
//
// This system allows queuing Vulkan resources for deferred deletion.
// Resources are destroyed in reverse order (LIFO) when execute is called.
//
// To add support for a new Vulkan resource type, add one line to the
// FOREACH_DESTROY_TYPE macro below.
///////////////////////////////////////////////////////////////////////////////

#define FOREACH_DESTROY_TYPE(X) \
	X(buffer,               VkBuffer,                vkDestroyBuffer,                device)   \
	X(image,                VkImage,                 vkDestroyImage,                 device)   \
	X(image_view,           VkImageView,             vkDestroyImageView,             device)   \
	X(sampler,              VkSampler,               vkDestroySampler,               device)   \
	X(framebuffer,          VkFramebuffer,           vkDestroyFramebuffer,           device)   \
	X(render_pass,          VkRenderPass,            vkDestroyRenderPass,            device)   \
	X(pipeline,             VkPipeline,              vkDestroyPipeline,              device)   \
	X(pipeline_layout,      VkPipelineLayout,        vkDestroyPipelineLayout,        device)   \
	X(pipeline_cache,       VkPipelineCache,         vkDestroyPipelineCache,         device)   \
	X(descriptor_set_layout,VkDescriptorSetLayout,   vkDestroyDescriptorSetLayout,   device)   \
	X(descriptor_pool,      VkDescriptorPool,        vkDestroyDescriptorPool,        device)   \
	X(shader_module,        VkShaderModule,          vkDestroyShaderModule,          device)   \
	X(command_pool,         VkCommandPool,           vkDestroyCommandPool,           device)   \
	X(fence,                VkFence,                 vkDestroyFence,                 device)   \
	X(semaphore,            VkSemaphore,             vkDestroySemaphore,             device)   \
	X(query_pool,           VkQueryPool,             vkDestroyQueryPool,             device)   \
	X(swapchain,            VkSwapchainKHR,          vkDestroySwapchainKHR,          device)   \
	X(surface,              VkSurfaceKHR,            vkDestroySurfaceKHR,            instance) \
	X(debug_messenger,      VkDebugUtilsMessengerEXT,vkDestroyDebugUtilsMessengerEXT,instance) \
	X(memory,               VkDeviceMemory,          vkFreeMemory,                   device)

typedef enum {
	#define MAKE_ENUM(name, type, func, owner) skr_destroy_type_##name,
	FOREACH_DESTROY_TYPE(MAKE_ENUM)
	#undef MAKE_ENUM
} skr_destroy_type_;

typedef struct {
	skr_destroy_type_ type;
	uint64_t          handle;
} skr_destroy_item_t;

///////////////////////////////////////////////////////////////////////////////

skr_destroy_list_t _skr_destroy_list_create(void) {
	return (skr_destroy_list_t){ .items = NULL, .count = 0, .capacity = 0 };
}

void _skr_destroy_list_free(skr_destroy_list_t* list) {
	if (!list) return;
	free(list->items);
	list->items    = NULL;
	list->count    = 0;
	list->capacity = 0;
}

static void _skr_destroy_list_ensure_capacity(skr_destroy_list_t* list, uint32_t required) {
	if (list->capacity >= required) return;

	uint32_t new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
	while (new_capacity < required) {
		new_capacity *= 2;
	}

	skr_destroy_item_t* new_items = realloc(list->items, new_capacity * sizeof(skr_destroy_item_t));
	if (!new_items) {
		skr_log(skr_log_critical, "Failed to resize destroy list");
		return;
	}

	list->items    = new_items;
	list->capacity = new_capacity;
}

static void _skr_destroy_list_add(skr_destroy_list_t* list, uint64_t handle, skr_destroy_type_ type){
	_skr_destroy_list_ensure_capacity(list, list->count + 1);

	skr_destroy_item_t* items = (skr_destroy_item_t*)list->items;
	items[list->count++] = (skr_destroy_item_t){
		.type   = type,
		.handle = handle,
	};
}

static void _skr_destroy_list_destroy(uint64_t handle, skr_destroy_type_ type) {
	switch (type) {
		#define MAKE_CASE(name, vk_type, destroy_func, owner) case skr_destroy_type_##name: destroy_func(_skr_vk.owner, (vk_type)handle, NULL); break;
		FOREACH_DESTROY_TYPE(MAKE_CASE)
		#undef MAKE_CASE
	}
}

#define MAKE_ADD_FUNCTION(name, vk_type, func, owner) \
void _skr_cmd_destroy_##name(skr_destroy_list_t* opt_list, vk_type handle) { \
	if (handle == VK_NULL_HANDLE) return; \
	if (opt_list == NULL) { _skr_cmd_ring_slot_t* active = _skr_cmd_get_thread()->active_cmd; opt_list = active ? &active->destroy_list : NULL; } \
	if (opt_list == NULL) { _skr_destroy_list_destroy(          (uint64_t)handle, skr_destroy_type_##name); } \
	else                  { _skr_destroy_list_add    (opt_list, (uint64_t)handle, skr_destroy_type_##name); } \
}
FOREACH_DESTROY_TYPE(MAKE_ADD_FUNCTION)
#undef MAKE_ADD_FUNCTION

void _skr_destroy_list_execute(skr_destroy_list_t* list) {
	// Execute in reverse order (LIFO - last in, first out)
	skr_destroy_item_t* items = (skr_destroy_item_t*)list->items;
	for (int32_t i = list->count - 1; i >= 0; i--)
		_skr_destroy_list_destroy(items[i].handle, items[i].type);
}

void _skr_destroy_list_clear(skr_destroy_list_t* list) {
	if (!list) return;
	list->count = 0;
}
