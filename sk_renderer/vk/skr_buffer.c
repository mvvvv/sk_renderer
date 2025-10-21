#include "_sk_renderer.h"
#include "skr_conversions.h"
#include "../skr_log.h"

#include <stdio.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Helper functions
///////////////////////////////////////////////////////////////////////////////

static uint32_t _skr_find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties mem_properties;
	vkGetPhysicalDeviceMemoryProperties(_skr_vk.physical_device, &mem_properties);

	for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
		if ((type_filter & (1 << i)) &&
		    (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}

	skr_log(skr_log_critical, "Failed to find suitable memory type");
	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Buffer creation and destruction
///////////////////////////////////////////////////////////////////////////////

skr_buffer_t skr_buffer_create(const void* data, uint32_t size_count, uint32_t size_stride,
                                skr_buffer_type_ type, skr_use_ use) {
	skr_buffer_t buffer = {0};
	buffer.size         = size_count * size_stride;
	buffer.type         = type;
	buffer.use          = use;

	// Determine buffer usage flags
	VkBufferUsageFlags usage = _skr_to_vk_buffer_usage(type);

	// Add transfer dst for initial data upload (unless dynamic)
	if (data != NULL && use != skr_use_dynamic) {
		usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	}

	// Create buffer
	if (vkCreateBuffer(_skr_vk.device, &(VkBufferCreateInfo){
		.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size        = buffer.size,
		.usage       = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	}, NULL, &buffer.buffer) != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to create buffer");
		return buffer;
	}

	// Allocate memory
	VkMemoryRequirements mem_requirements;
	vkGetBufferMemoryRequirements(_skr_vk.device, buffer.buffer, &mem_requirements);

	VkMemoryPropertyFlags mem_properties;
	if (use == skr_use_dynamic) {
		// Dynamic buffers use host-visible, host-coherent memory
		mem_properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	} else {
		// Static buffers use device-local memory
		mem_properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	}

	VkMemoryAllocateInfo alloc_info = {
		.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize  = mem_requirements.size,
		.memoryTypeIndex = _skr_find_memory_type(mem_requirements.memoryTypeBits, mem_properties),
	};

	if (vkAllocateMemory(_skr_vk.device, &alloc_info, NULL, &buffer.memory) != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to allocate buffer memory");
		vkDestroyBuffer(_skr_vk.device, buffer.buffer, NULL);
		buffer.buffer = VK_NULL_HANDLE;
		return buffer;
	}

	vkBindBufferMemory(_skr_vk.device, buffer.buffer, buffer.memory, 0);

	// Upload initial data
	if (data != NULL) {
		if (use == skr_use_dynamic) {
			// Direct map and copy for dynamic buffers
			void* mapped;
			vkMapMemory(_skr_vk.device, buffer.memory, 0, buffer.size, 0, &mapped);
			memcpy(mapped, data, buffer.size);
			vkUnmapMemory(_skr_vk.device, buffer.memory);
		} else {
			// Use staging buffer for static buffers
			VkBuffer staging_buffer;
			vkCreateBuffer(_skr_vk.device, &(VkBufferCreateInfo){
				.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size        = buffer.size,
				.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			}, NULL, &staging_buffer);

			VkMemoryRequirements staging_mem_req;
			vkGetBufferMemoryRequirements(_skr_vk.device, staging_buffer, &staging_mem_req);

			VkDeviceMemory staging_memory;
			vkAllocateMemory(_skr_vk.device, &(VkMemoryAllocateInfo){
				.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize  = staging_mem_req.size,
				.memoryTypeIndex = _skr_find_memory_type(staging_mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
			}, NULL, &staging_memory);
			vkBindBufferMemory(_skr_vk.device, staging_buffer, staging_memory, 0);

			// Copy data to staging buffer
			void* mapped;
			vkMapMemory(_skr_vk.device, staging_memory, 0, buffer.size, 0, &mapped);
			memcpy(mapped, data, buffer.size);
			vkUnmapMemory(_skr_vk.device, staging_memory);

			// Copy staging buffer to device buffer
			VkCommandBuffer cmd;
			vkAllocateCommandBuffers(_skr_vk.device, &(VkCommandBufferAllocateInfo){
				.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool        = _skr_vk.command_pool,
				.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1,
			}, &cmd);

			vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			});

			vkCmdCopyBuffer(cmd, staging_buffer, buffer.buffer, 1, &(VkBufferCopy){
				.size = buffer.size,
			});

			vkEndCommandBuffer(cmd);

			vkQueueSubmit(_skr_vk.graphics_queue, 1, &(VkSubmitInfo){
				.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount = 1,
				.pCommandBuffers    = &cmd,
			}, VK_NULL_HANDLE);
			vkQueueWaitIdle(_skr_vk.graphics_queue);

			vkFreeCommandBuffers(_skr_vk.device, _skr_vk.command_pool, 1, &cmd);
			vkDestroyBuffer     (_skr_vk.device, staging_buffer, NULL);
			vkFreeMemory        (_skr_vk.device, staging_memory, NULL);
		}
	}
 
	// Keep dynamic buffers mapped
	if (use == skr_use_dynamic) {
		vkMapMemory(_skr_vk.device, buffer.memory, 0, buffer.size, 0, &buffer.mapped);
	}

	return buffer;
}

bool skr_buffer_is_valid(const skr_buffer_t* buffer) {
	return buffer && buffer->buffer != VK_NULL_HANDLE;
}

void skr_buffer_set(skr_buffer_t* buffer, const void* data, uint32_t size_bytes) {
	if (!buffer || !data) return;

	if (buffer->use == skr_use_dynamic && buffer->mapped) {
		memcpy(buffer->mapped, data, size_bytes < buffer->size ? size_bytes : buffer->size);
	} else {
		skr_log(skr_log_critical, "skr_buffer_set only supports dynamic buffers");
	}
}

uint32_t skr_buffer_get_size(const skr_buffer_t* buffer) {
	return buffer ? buffer->size : 0;
}

void skr_buffer_set_name(skr_buffer_t* buffer, const char* name) {
	if (!buffer || buffer->buffer == VK_NULL_HANDLE) return;
	_skr_set_debug_name(VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer->buffer, name);
}

void skr_buffer_destroy(skr_buffer_t* buffer) {
	if (!buffer || buffer->buffer == VK_NULL_HANDLE) return;

	if (buffer->mapped) {
		vkUnmapMemory(_skr_vk.device, buffer->memory);
		buffer->mapped = NULL;
	}

	_skr_command_context_t ctx;
	if (_skr_command_try_get_active(&ctx)){
		_skr_destroy_list_add_buffer(ctx.destroy_list, buffer->buffer);
		_skr_destroy_list_add_memory(ctx.destroy_list, buffer->memory);
	} else {
		vkDestroyBuffer(_skr_vk.device, buffer->buffer, NULL);
		vkFreeMemory   (_skr_vk.device, buffer->memory, NULL);
	}

	memset(buffer, 0, sizeof(skr_buffer_t));
}
