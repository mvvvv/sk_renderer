// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "sk_renderer.h"
#include "_sk_renderer.h"

#include "skr_vulkan.h"
#include "skr_conversions.h"
#include "../skr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

///////////////////////////////////////////////////////////////////////////////
// Helper functions
///////////////////////////////////////////////////////////////////////////////

// Find memory type index with required properties, returns UINT32_MAX if not found
static uint32_t _skr_find_memory_type(VkMemoryRequirements mem_requirements, VkMemoryPropertyFlags required_props) {
	VkPhysicalDeviceMemoryProperties mem_properties;
	vkGetPhysicalDeviceMemoryProperties(_skr_vk.physical_device, &mem_properties);

	for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
		if ((mem_requirements.memoryTypeBits & (1 << i)) &&
		    (mem_properties.memoryTypes[i].propertyFlags & required_props) == required_props) {
			return i;
		}
	}
	return UINT32_MAX;
}

// Allocate device memory for an image, trying lazily-allocated first for transient attachments
static VkDeviceMemory _skr_allocate_image_memory(VkImage image, bool is_transient_attachment, VkDeviceMemory* out_memory) {
	VkMemoryRequirements mem_requirements;
	vkGetImageMemoryRequirements(_skr_vk.device, image, &mem_requirements);

	uint32_t memory_type_index = UINT32_MAX;

	// For transient MSAA attachments, prefer lazily allocated memory
	if (is_transient_attachment) {
		memory_type_index = _skr_find_memory_type(mem_requirements, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
	}

	// Fallback to device local memory
	if (memory_type_index == UINT32_MAX) {
		memory_type_index = _skr_find_memory_type(mem_requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	}

	if (memory_type_index == UINT32_MAX) {
		return VK_NULL_HANDLE;
	}

	VkMemoryAllocateInfo alloc_info = {
		.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize  = mem_requirements.size,
		.memoryTypeIndex = memory_type_index,
	};

	if (vkAllocateMemory(_skr_vk.device, &alloc_info, NULL, out_memory) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}

	return *out_memory;
}

// Create staging buffer and memory for texture uploads
typedef struct {
	VkBuffer       buffer;
	VkDeviceMemory memory;
	void*          mapped_data;
	bool           valid;
} staging_buffer_t;

static staging_buffer_t _skr_create_staging_buffer(VkDeviceSize size) {
	staging_buffer_t result = {0};

	VkBufferCreateInfo buffer_info = {
		.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size        = size,
		.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	if (vkCreateBuffer(_skr_vk.device, &buffer_info, NULL, &result.buffer) != VK_SUCCESS) {
		return result;
	}

	VkMemoryRequirements mem_requirements;
	vkGetBufferMemoryRequirements(_skr_vk.device, result.buffer, &mem_requirements);

	uint32_t memory_type_index = _skr_find_memory_type(mem_requirements,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	if (memory_type_index == UINT32_MAX) {
		vkDestroyBuffer(_skr_vk.device, result.buffer, NULL);
		return result;
	}

	VkMemoryAllocateInfo alloc_info = {
		.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize  = mem_requirements.size,
		.memoryTypeIndex = memory_type_index,
	};

	if (vkAllocateMemory(_skr_vk.device, &alloc_info, NULL, &result.memory) != VK_SUCCESS) {
		vkDestroyBuffer(_skr_vk.device, result.buffer, NULL);
		return result;
	}

	vkBindBufferMemory(_skr_vk.device, result.buffer, result.memory, 0);

	if (vkMapMemory(_skr_vk.device, result.memory, 0, size, 0, &result.mapped_data) != VK_SUCCESS) {
		vkFreeMemory(_skr_vk.device, result.memory, NULL);
		vkDestroyBuffer(_skr_vk.device, result.buffer, NULL);
		return result;
	}

	result.valid = true;
	return result;
}

static void _skr_destroy_staging_buffer(staging_buffer_t* staging) {
	if (!staging) return;
	if (staging->mapped_data) vkUnmapMemory(_skr_vk.device, staging->memory);
	if (staging->buffer)      vkDestroyBuffer(_skr_vk.device, staging->buffer, NULL);
	if (staging->memory)      vkFreeMemory(_skr_vk.device, staging->memory, NULL);
	*staging = (staging_buffer_t){0};
}

// Begin a one-time command buffer for immediate operations
static VkCommandBuffer _skr_begin_single_time_commands(void) {
	VkCommandBuffer cmd;
	VkCommandBufferAllocateInfo alloc_info = {
		.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool        = _skr_vk.command_pool,
		.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};

	if (vkAllocateCommandBuffers(_skr_vk.device, &alloc_info, &cmd) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}

	VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
		vkFreeCommandBuffers(_skr_vk.device, _skr_vk.command_pool, 1, &cmd);
		return VK_NULL_HANDLE;
	}

	return cmd;
}

// End and submit a one-time command buffer, wait for completion
static void _skr_end_single_time_commands(VkCommandBuffer cmd) {
	if (!cmd) return;

	vkEndCommandBuffer(cmd);

	VkSubmitInfo submit_info = {
		.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers    = &cmd,
	};

	vkQueueSubmit(_skr_vk.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
	vkQueueWaitIdle(_skr_vk.graphics_queue);
	vkFreeCommandBuffers(_skr_vk.device, _skr_vk.command_pool, 1, &cmd);
}

// Transition image layout with a pipeline barrier (low-level helper)
static void _skr_transition_image_layout(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect_mask,
                                          uint32_t base_mip, uint32_t mip_count, uint32_t layer_count,
                                          VkImageLayout old_layout, VkImageLayout new_layout,
                                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                                          VkAccessFlags src_access, VkAccessFlags dst_access) {
	VkImageMemoryBarrier barrier = {
		.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.oldLayout           = old_layout,
		.newLayout           = new_layout,
		.srcAccessMask       = src_access,
		.dstAccessMask       = dst_access,
		.image               = image,
		.subresourceRange    = {
			.aspectMask     = aspect_mask,
			.baseMipLevel   = base_mip,
			.levelCount     = mip_count,
			.baseArrayLayer = 0,
			.layerCount     = layer_count,
		},
	};

	vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

// Helper: Convert layout to typical source stage flags
static VkPipelineStageFlags _layout_to_src_stage(VkImageLayout layout) {
	switch (layout) {
		case VK_IMAGE_LAYOUT_UNDEFINED:
			return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			return VK_PIPELINE_STAGE_TRANSFER_BIT;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			return VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		case VK_IMAGE_LAYOUT_GENERAL:
			return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
			return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		default:
			return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	}
}

// Helper: Convert layout to typical access flags
static VkAccessFlags _layout_to_access_flags(VkImageLayout layout) {
	switch (layout) {
		case VK_IMAGE_LAYOUT_UNDEFINED:
			return 0;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			return VK_ACCESS_TRANSFER_READ_BIT;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			return VK_ACCESS_TRANSFER_WRITE_BIT;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			return VK_ACCESS_SHADER_READ_BIT;
		case VK_IMAGE_LAYOUT_GENERAL:
			return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
			return 0;
		default:
			return 0;
	}
}

#ifdef SKR_DEBUG
// Helper: Layout to string for debug logging
static const char* _layout_to_string(VkImageLayout layout) {
	switch (layout) {
		case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
		case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT";
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_STENCIL_ATTACHMENT";
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DEPTH_STENCIL_READ_ONLY";
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ_ONLY";
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC";
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST";
		case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT_SRC";
		default: return "UNKNOWN";
	}
}
#endif

///////////////////////////////////////////////////////////////////////////////
// Automatic Layout Transition System
///////////////////////////////////////////////////////////////////////////////

// Check if texture needs transition for given type (without requiring command buffer)
bool _skr_tex_needs_transition(const skr_tex_t* tex, uint8_t type) {
	if (!tex || !tex->image) return false;

	// Transient discard textures always need transitions (conceptually always UNDEFINED)
	if (tex->is_transient_discard) return true;

	// Determine target layout based on type
	VkImageLayout target_layout;
	if (type == 1) {  // storage
		target_layout = VK_IMAGE_LAYOUT_GENERAL;
	} else {  // shader_read (type == 0)
		// Storage images use GENERAL layout, regular textures use SHADER_READ_ONLY_OPTIMAL
		target_layout = (tex->flags & skr_tex_flags_compute)
			? VK_IMAGE_LAYOUT_GENERAL
			: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	// Check if already in target layout
	return tex->current_layout != target_layout;
}

// General-purpose automatic transition - tracks state and inserts barrier if needed
void _skr_tex_transition(VkCommandBuffer cmd, skr_tex_t* tex, VkImageLayout new_layout,
                          VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
	if (!tex || !tex->image) return;

	// For transient discard textures (non-readable depth/MSAA), always use UNDEFINED as old layout
	VkImageLayout old_layout = tex->is_transient_discard ? VK_IMAGE_LAYOUT_UNDEFINED : tex->current_layout;

	// Skip if already in target layout (unless it's a transient discard texture)
	if (!tex->is_transient_discard && tex->current_layout == new_layout) {
		return;
	}

#ifdef SKR_DEBUG
	if (tex->current_layout != VK_IMAGE_LAYOUT_UNDEFINED && tex->current_layout != old_layout) {
		skr_logf(skr_log_warning, "Texture layout mismatch: tracked=%s, using=%s for transition to %s",
			_layout_to_string(tex->current_layout), _layout_to_string(old_layout), _layout_to_string(new_layout));
	}
#endif

	// Determine source stage and access from old layout
	VkPipelineStageFlags src_stage  = _layout_to_src_stage(old_layout);
	VkAccessFlags        src_access = _layout_to_access_flags(old_layout);

	// Perform transition
	_skr_transition_image_layout(cmd, tex->image, tex->aspect_mask,
		0, tex->mip_levels, tex->layer_count,
		old_layout, new_layout,
		src_stage, dst_stage,
		src_access, dst_access);

	// Update tracked state (unless it's transient discard - always stays UNDEFINED conceptually)
	if (!tex->is_transient_discard) {
		tex->current_layout = new_layout;
	}
	tex->first_use = false;
}

// Specialized: Transition for shader read (most common case)
void _skr_tex_transition_for_shader_read(VkCommandBuffer cmd, skr_tex_t* tex, VkPipelineStageFlags dst_stage) {
	if (!tex) return;

	// Storage images use GENERAL layout, regular textures use SHADER_READ_ONLY_OPTIMAL
	VkImageLayout target_layout = (tex->flags & skr_tex_flags_compute)
		? VK_IMAGE_LAYOUT_GENERAL
		: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	_skr_tex_transition(cmd, tex, target_layout, dst_stage, VK_ACCESS_SHADER_READ_BIT);
}

// Specialized: Transition for storage image (compute RWTexture)
void _skr_tex_transition_for_storage(VkCommandBuffer cmd, skr_tex_t* tex) {
	if (!tex) return;
	_skr_tex_transition(cmd, tex, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

// Notify the system that a render pass has performed an implicit layout transition
// This updates tracked state without issuing a barrier
void _skr_tex_transition_notify_layout(skr_tex_t* tex, VkImageLayout new_layout) {
	if (!tex) return;

	// Don't update transient discard textures - they conceptually stay in UNDEFINED
	if (!tex->is_transient_discard) {
		tex->current_layout = new_layout;
	}
	tex->first_use = false;
}

// Queue family ownership transfer (for future async upload)
void _skr_tex_transition_queue_family(VkCommandBuffer cmd, skr_tex_t* tex,
                                     uint32_t src_queue_family, uint32_t dst_queue_family,
                                     VkImageLayout layout) {
	if (!tex || !tex->image || src_queue_family == dst_queue_family) return;

	VkImageLayout old_layout = tex->is_transient_discard ? VK_IMAGE_LAYOUT_UNDEFINED : tex->current_layout;
	VkAccessFlags src_access = _layout_to_access_flags(old_layout);
	VkAccessFlags dst_access = _layout_to_access_flags(layout);

	VkImageMemoryBarrier barrier = {
		.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcQueueFamilyIndex = src_queue_family,
		.dstQueueFamilyIndex = dst_queue_family,
		.oldLayout           = old_layout,
		.newLayout           = layout,
		.srcAccessMask       = src_access,
		.dstAccessMask       = dst_access,
		.image               = tex->image,
		.subresourceRange    = {
			.aspectMask     = tex->aspect_mask,
			.baseMipLevel   = 0,
			.levelCount     = tex->mip_levels,
			.baseArrayLayer = 0,
			.layerCount     = tex->layer_count,
		},
	};

	VkPipelineStageFlags src_stage = _layout_to_src_stage(old_layout);
	VkPipelineStageFlags dst_stage = _layout_to_src_stage(layout);

	vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);

	// Update tracked state
	if (!tex->is_transient_discard) {
		tex->current_layout = layout;
	}
	tex->current_queue_family = dst_queue_family;
	tex->first_use = false;
}

///////////////////////////////////////////////////////////////////////////////

static void _skr_tex_generate_mips_blit  (skr_tex_t* tex, int32_t mip_levels);
static void _skr_tex_generate_mips_render(skr_tex_t* tex, int32_t mip_levels, const skr_shader_t* fragment_shader);

///////////////////////////////////////////////////////////////////////////////

skr_tex_t skr_tex_create(skr_tex_fmt_ format, skr_tex_flags_ flags, skr_tex_sampler_t sampler,
                          skr_vec3i_t size, int32_t multisample, int32_t mip_count, const void* opt_tex_data) {
	skr_tex_t tex = {0};
	tex.size             = size;
	tex.flags            = flags;
	tex.sampler_settings = sampler;

	// Validate parameters
	if (size.x <= 0 || size.y <= 0 || size.z <= 0) {
		skr_log(skr_log_warning, "Invalid texture size");
		return (skr_tex_t){};
	}

	tex.format = format;
	VkFormat vk_format = _skr_to_vk_tex_fmt(format);
	if (vk_format == VK_FORMAT_UNDEFINED) {
		return (skr_tex_t){};
	}

	// Determine image type, layer count, and normalize counts
	VkImageType image_type;
	if (tex.flags & skr_tex_flags_3d) {
		image_type  = VK_IMAGE_TYPE_3D;
		tex.layer_count = 1;
	} else if (tex.flags & skr_tex_flags_array) {
		image_type  = VK_IMAGE_TYPE_2D;
		tex.layer_count = size.z;
	} else if (tex.flags & skr_tex_flags_cubemap) {
		image_type  = VK_IMAGE_TYPE_2D;
		tex.layer_count = 6;
	} else {
		image_type = (size.z > 1) ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_2D;
		tex.layer_count = 1;
	}

	// Normalize mip count (default to 1 if not specified or zero)
	tex.mip_levels = mip_count <= 0 ? 1 : mip_count;

	// Normalize sample count
	tex.samples = (multisample > 1) ? (VkSampleCountFlagBits)multisample : VK_SAMPLE_COUNT_1_BIT;

	// Determine usage flags
	VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT; // Always allow sampling for readable textures
	if (tex.flags & skr_tex_flags_readable) {
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	// Check if this is a depth format
	bool is_depth = (format == skr_tex_fmt_depth16 || format == skr_tex_fmt_depth32 || format == skr_tex_fmt_depth32s8 || format == skr_tex_fmt_depth24s8 || format == skr_tex_fmt_depth16s8);

	// Determine aspect mask based on format
	tex.aspect_mask  = 0;
	if (is_depth)                           { tex.aspect_mask  |= VK_IMAGE_ASPECT_DEPTH_BIT;   }
	if (_skr_format_has_stencil(vk_format)) { tex.aspect_mask  |= VK_IMAGE_ASPECT_STENCIL_BIT; }
	if (tex.aspect_mask == 0)               { tex.aspect_mask   = VK_IMAGE_ASPECT_COLOR_BIT;   }


	// For MSAA attachments, add transient bit for in-tile resolve optimization
	// But only if the texture is NOT readable (transient means no memory backing)
	bool is_msaa_attachment = tex.samples > VK_SAMPLE_COUNT_1_BIT && (tex.flags & skr_tex_flags_writeable) && !(tex.flags & skr_tex_flags_readable);

	if (tex.flags & skr_tex_flags_writeable) {
		if (is_depth) {
			usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		} else {
			usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			// TRANSIENT_ATTACHMENT_BIT can't be combined with TRANSFER_DST_BIT
			if (!is_msaa_attachment) {
				usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			}
		}
	}
	if (opt_tex_data) {
		usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT; // Need to upload data
	}

	// Only use transient attachment if format+usage combination is supported
	if (is_msaa_attachment) {
		VkImageUsageFlags test_usage = usage | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
		test_usage &= ~(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

		VkImageFormatProperties format_props;
		VkResult result = vkGetPhysicalDeviceImageFormatProperties(
			_skr_vk.physical_device,
			vk_format,
			image_type,
			VK_IMAGE_TILING_OPTIMAL,
			test_usage,
			0,
			&format_props);

		if (result == VK_SUCCESS) {
			usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
			// Remove SAMPLED_BIT and TRANSFER_DST_BIT for transient attachments
			usage &= ~(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		}
		// If not supported, just use regular memory (no transient optimization)
	}

	// For compute shader storage images (RWTexture2D)
	if (tex.flags & skr_tex_flags_compute) {
		usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	}

	// For mipmap generation
	if (tex.flags & skr_tex_flags_gen_mips) {
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

		// Check if format supports STORAGE_BIT (needed for compute-based mipmap filters)
		VkFormatProperties format_props;
		vkGetPhysicalDeviceFormatProperties(_skr_vk.physical_device, vk_format, &format_props);
		if (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
			usage |= VK_IMAGE_USAGE_STORAGE_BIT;
		}

		// Add color attachment usage for render-based mipmap generation
		if (!is_depth) {
			usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		}

		// Auto-calculate mip count if not specified or is 1
		if (tex.mip_levels == 1) {
			int32_t max_dim = size.x > size.y ? size.x : size.y;
			tex.mip_levels = (int32_t)floor(log2(max_dim)) + 1;
		}
	}

	// Create image
	VkImageCreateInfo image_info = {
		.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType     = image_type,
		.format        = vk_format,
		.extent        = { .width = size.x, .height = size.y, .depth = (tex.flags & skr_tex_flags_3d) ? size.z : 1 },
		.mipLevels     = tex.mip_levels,
		.arrayLayers   = tex.layer_count,
		.samples       = tex.samples,
		.tiling        = VK_IMAGE_TILING_OPTIMAL,
		.usage         = usage,
		.sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.flags         = (tex.flags & skr_tex_flags_cubemap) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
	};

	if (vkCreateImage(_skr_vk.device, &image_info, NULL, &tex.image) != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to create texture image");
		return tex;
	}

	// Allocate memory using helper
	if (_skr_allocate_image_memory(tex.image, is_msaa_attachment, &tex.memory) == VK_NULL_HANDLE) {
		skr_log(skr_log_critical, "Failed to allocate texture memory");
		vkDestroyImage(_skr_vk.device, tex.image, NULL);
		tex.image = VK_NULL_HANDLE;
		return tex;
	}

	vkBindImageMemory(_skr_vk.device, tex.image, tex.memory, 0);

	// Upload texture data if provided (or just transition to shader read layout)
	if (opt_tex_data) {
		// Calculate data size and create staging buffer
		uint32_t           pixel_size = _skr_tex_fmt_to_size(format);
		VkDeviceSize       data_size  = size.x * size.y * size.z * pixel_size;
		staging_buffer_t   staging    = _skr_create_staging_buffer(data_size);

		if (!staging.valid) {
			skr_log(skr_log_critical, "Failed to create staging buffer for texture upload");
			vkFreeMemory(_skr_vk.device, tex.memory, NULL);
			vkDestroyImage(_skr_vk.device, tex.image, NULL);
			tex.image  = VK_NULL_HANDLE;
			tex.memory = VK_NULL_HANDLE;
			return tex;
		}

		// Copy texture data to staging buffer
		memcpy(staging.mapped_data, opt_tex_data, data_size);

		// Create command buffer and upload
		VkCommandBuffer cmd = _skr_begin_single_time_commands();
		if (!cmd) {
			skr_log(skr_log_critical, "Failed to create command buffer for texture upload");
			_skr_destroy_staging_buffer(&staging);
			vkFreeMemory  (_skr_vk.device, tex.memory, NULL);
			vkDestroyImage(_skr_vk.device, tex.image,  NULL);
			tex.image  = VK_NULL_HANDLE;
			tex.memory = VK_NULL_HANDLE;
			return tex;
		}

		// Transition: UNDEFINED -> TRANSFER_DST (using automatic system)
		_skr_tex_transition(cmd, &tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

		// Copy buffer to image (all layers at once for arrays/cubemaps)
		vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkBufferImageCopy){
			.bufferOffset      = 0,
			.bufferRowLength   = 0,
			.bufferImageHeight = 0,
			.imageSubresource  = {
				.aspectMask     = tex.aspect_mask,
				.mipLevel       = 0,
				.baseArrayLayer = 0,
				.layerCount     = tex.layer_count,
			},
			.imageOffset = {0, 0, 0},
			.imageExtent = {size.x, size.y, (tex.flags & skr_tex_flags_3d) ? size.z : 1},
		});

		// Transition: TRANSFER_DST -> SHADER_READ_ONLY (using automatic system)
		// Include compute shader stage if texture has compute flag
		VkPipelineStageFlags shader_stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		if (tex.flags & skr_tex_flags_compute) {
			shader_stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		}
		_skr_tex_transition_for_shader_read(cmd, &tex, shader_stages);

		_skr_end_single_time_commands(cmd);
		_skr_destroy_staging_buffer(&staging);
	} else if (!is_msaa_attachment && !(tex.flags & skr_tex_flags_writeable)) {
		// No data provided, transition to appropriate layout for read-only textures
		// Skip for transient MSAA attachments - they don't need initial layout transition
		// Skip for writeable textures - let the first render pass handle the transition

		VkCommandBuffer cmd = _skr_begin_single_time_commands();
		if (cmd) {
			// Use automatic transition system - handles storage vs regular textures
			if (tex.flags & skr_tex_flags_compute) { _skr_tex_transition_for_storage    (cmd, &tex); } 
			else                                   { _skr_tex_transition_for_shader_read(cmd, &tex, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT); }
			_skr_end_single_time_commands(cmd);
		}
	}

	// Create image view
	VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
	if      (tex.flags & skr_tex_flags_3d     ) view_type = VK_IMAGE_VIEW_TYPE_3D;
	else if (tex.flags & skr_tex_flags_cubemap) view_type = VK_IMAGE_VIEW_TYPE_CUBE;
	else if (tex.flags & skr_tex_flags_array  ) view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;

	VkImageViewCreateInfo view_info = {
		.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image    = tex.image,
		.viewType = view_type,
		.format   = vk_format,
		.subresourceRange = {
			.aspectMask     = tex.aspect_mask,
			.baseMipLevel   = 0,
			.levelCount     = image_info.mipLevels,
			.baseArrayLayer = 0,
			.layerCount     = image_info.arrayLayers,
		},
	};

	if (vkCreateImageView(_skr_vk.device, &view_info, NULL, &tex.view) != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to create texture image view");
		vkFreeMemory  (_skr_vk.device, tex.memory, NULL);
		vkDestroyImage(_skr_vk.device, tex.image,  NULL);
		tex.image  = VK_NULL_HANDLE;
		tex.memory = VK_NULL_HANDLE;
		return tex;
	}

	// Store texture properties
	tex.sampler              = _skr_sampler_create_vk(sampler);

	// Initialize layout tracking
	tex.current_layout       = VK_IMAGE_LAYOUT_UNDEFINED;
	tex.current_queue_family = _skr_vk.graphics_queue_family;
	tex.first_use            = true;
	// Transient discard optimization for non-readable depth/MSAA (tile GPU optimization)
	tex.is_transient_discard = (is_msaa_attachment || (is_depth && !(flags & skr_tex_flags_readable)));

	return tex;
}

void skr_tex_destroy(skr_tex_t* tex) {
	if (!tex) return;
	if (tex->framebuffer       != VK_NULL_HANDLE) vkDestroyFramebuffer(_skr_vk.device, tex->framebuffer,       NULL);
	if (tex->framebuffer_depth != VK_NULL_HANDLE) vkDestroyFramebuffer(_skr_vk.device, tex->framebuffer_depth, NULL);
	if (tex->sampler           != VK_NULL_HANDLE) vkDestroySampler    (_skr_vk.device, tex->sampler,           NULL);
	if (tex->view              != VK_NULL_HANDLE) vkDestroyImageView  (_skr_vk.device, tex->view,              NULL);
	if (tex->image             != VK_NULL_HANDLE) vkDestroyImage      (_skr_vk.device, tex->image,             NULL);
	if (tex->memory            != VK_NULL_HANDLE) vkFreeMemory        (_skr_vk.device, tex->memory,            NULL);
	*tex = (skr_tex_t){};
}

bool skr_tex_is_valid(const skr_tex_t* tex) {
	return tex && tex->image != VK_NULL_HANDLE;
}

skr_vec3i_t skr_tex_get_size(const skr_tex_t* tex) {
	return tex ? tex->size : (skr_vec3i_t){0, 0, 0};
}

skr_tex_fmt_ skr_tex_get_format(const skr_tex_t* tex) {
	return tex ? tex->format : skr_tex_fmt_none;
}

skr_tex_flags_ skr_tex_get_flags(const skr_tex_t* tex) {
	return tex ? tex->flags : 0;
}

int32_t skr_tex_get_multisample(const skr_tex_t* tex) {
	return tex ? tex->samples : VK_SAMPLE_COUNT_1_BIT;
}

skr_tex_sampler_t skr_tex_get_sampler(const skr_tex_t* tex) {
	return tex ? tex->sampler_settings : (skr_tex_sampler_t){0};
}

void skr_tex_set_name(skr_tex_t* tex, const char* name) {
	if (!tex || tex->image == VK_NULL_HANDLE) return;

	_skr_set_debug_name(VK_OBJECT_TYPE_IMAGE, (uint64_t)tex->image, name);

	// Also name the image view if it exists
	if (tex->view != VK_NULL_HANDLE) {
		char view_name[256];
		snprintf(view_name, sizeof(view_name), "%s_view", name);
		_skr_set_debug_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t)tex->view, view_name);
	}
}

void skr_tex_generate_mips(skr_tex_t* tex, const skr_shader_t* opt_shader) {
	if (!skr_tex_is_valid(tex)) {
		skr_log(skr_log_warning, "Cannot generate mipmaps for invalid texture");
		return;
	}

	// Calculate mip levels
	int32_t max_dim = tex->size.x > tex->size.y ? tex->size.x : tex->size.y;
	int32_t mip_levels = (int32_t)floor(log2(max_dim)) + 1;

	if (mip_levels <= 1) {
		skr_log(skr_log_info, "Texture only has 1 mip level, nothing to generate");
		return;
	}

	// Route to appropriate implementation
	// If a custom shader is provided, use render-based mipmap generation (fragment shader)
	// Otherwise fall back to simple blit
	if (opt_shader == NULL) {
		_skr_tex_generate_mips_blit(tex, mip_levels);
	} else {
		_skr_tex_generate_mips_render(tex, mip_levels, opt_shader);
	}
}

static void _skr_tex_generate_mips_blit(skr_tex_t* tex, int32_t mip_levels) {
	// Check format support for blit operations
	VkFormatProperties format_properties;
	VkFormat           vk_format = _skr_to_vk_tex_fmt(tex->format);
	vkGetPhysicalDeviceFormatProperties(_skr_vk.physical_device, vk_format, &format_properties);

	if (!(format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
	    !(format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
		skr_log(skr_log_critical, "Texture format doesn't support blit operations for mipmap generation");
		return;
	}

	// Check if format supports linear filtering during blit
	VkFilter filter_mode = VK_FILTER_LINEAR;
	if (!(format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
		skr_log(skr_log_info, "Format doesn't support linear filtering, using nearest");
		filter_mode = VK_FILTER_NEAREST;
	}

	VkCommandBuffer cmd = _skr_begin_single_time_commands();
	if (!cmd) {
		skr_log(skr_log_critical, "Failed to create command buffer for mipmap generation");
		return;
	}

	// Transition mip 0 to TRANSFER_SRC_OPTIMAL (automatic system tracks current layout)
	_skr_tex_transition(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);

	// Generate each mip level by blitting from the previous level
	int32_t mip_width  = tex->size.x;
	int32_t mip_height = tex->size.y;

	for (int32_t i = 1; i < mip_levels; i++) {
		int32_t next_mip_width  = mip_width  > 1 ? mip_width  / 2 : 1;
		int32_t next_mip_height = mip_height > 1 ? mip_height / 2 : 1;

		// Transition mip i from UNDEFINED to TRANSFER_DST_OPTIMAL
		_skr_transition_image_layout(cmd, tex->image, tex->aspect_mask, i, 1, tex->layer_count,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, VK_ACCESS_TRANSFER_WRITE_BIT);

		// Blit from mip i-1 to mip i (for all layers)
		VkImageBlit blit = {
			.srcSubresource = {
				.aspectMask     = tex->aspect_mask,
				.mipLevel       = i - 1,
				.baseArrayLayer = 0,
				.layerCount     = tex->layer_count,
			},
			.srcOffsets[0] = {0, 0, 0},
			.srcOffsets[1] = {mip_width, mip_height, 1},
			.dstSubresource = {
				.aspectMask     = tex->aspect_mask,
				.mipLevel       = i,
				.baseArrayLayer = 0,
				.layerCount     = tex->layer_count,
			},
			.dstOffsets[0] = {0, 0, 0},
			.dstOffsets[1] = {next_mip_width, next_mip_height, 1},
		};

		vkCmdBlitImage(cmd,
			tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, filter_mode);

		// Transition mip i from TRANSFER_DST to TRANSFER_SRC for next iteration
		_skr_transition_image_layout(cmd, tex->image, tex->aspect_mask, i, 1, tex->layer_count,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

		mip_width  = next_mip_width;
		mip_height = next_mip_height;
	}

	// Transition back to shader read layout (automatic system handles this)
	_skr_tex_transition_for_shader_read(cmd, tex, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	_skr_end_single_time_commands(cmd);
}

static void _skr_tex_generate_mips_render(skr_tex_t* tex, int32_t mip_levels, const skr_shader_t* fragment_shader) {
	if (!skr_shader_is_valid(fragment_shader)) {
		skr_log(skr_log_warning, "Invalid fragment shader provided for mipmap generation");
		return;
	}

	skr_bind_t bind_source  = skr_shader_get_bind(fragment_shader, "src_tex");
	skr_bind_t bind_globals = skr_shader_get_bind(fragment_shader, "$Global"); // optional
	if ((bind_source.stage_bits & skr_stage_pixel) == 0) {
		skr_log(skr_log_warning, "Mip shader missing 'src_tex'");
		return;
	}

	_skr_command_context_t ctx = _skr_command_acquire();
	if (!ctx.cmd) {
		skr_log(skr_log_warning, "Failed to acquire command buffer for mipmap generation");
		return;
	}
	VkCommandBuffer cmd = ctx.cmd;

	// Create a simple render pass for rendering to a single color attachment
	VkFormat     format = _skr_to_vk_tex_fmt(tex->format);
	VkRenderPass render_pass = VK_NULL_HANDLE;
	{
		VkAttachmentDescription color_attachment = {
			.format         = format,
			.samples        = VK_SAMPLE_COUNT_1_BIT,
			.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};

		VkAttachmentReference color_ref = {
			.attachment = 0,
			.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};

		VkSubpassDescription subpass = {
			.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 1,
			.pColorAttachments    = &color_ref,
		};

		VkRenderPassCreateInfo rp_info = {
			.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments    = &color_attachment,
			.subpassCount    = 1,
			.pSubpasses      = &subpass,
		};

		if (vkCreateRenderPass(_skr_vk.device, &rp_info, NULL, &render_pass) != VK_SUCCESS) {
			skr_log(skr_log_critical, "Failed to create render pass for mipmap generation");
			_skr_command_release(cmd);
			return;
		}
	}

	// Create pipeline for fullscreen triangle rendering
	VkPipeline            pipeline          = VK_NULL_HANDLE;
	VkPipelineLayout      pipeline_layout   = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
	{
		descriptor_layout = _skr_shader_make_layout(fragment_shader->meta, skr_stage_pixel);

		VkPipelineLayoutCreateInfo pipeline_layout_info = {
			.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts    = &descriptor_layout,
		};

		if (vkCreatePipelineLayout(_skr_vk.device, &pipeline_layout_info, NULL, &pipeline_layout) != VK_SUCCESS) {
			skr_log(skr_log_critical, "Failed to create pipeline layout for mipmap generation");
			vkDestroyDescriptorSetLayout(_skr_vk.device, descriptor_layout, NULL);
			vkDestroyRenderPass         (_skr_vk.device, render_pass, NULL);
			_skr_command_release(cmd);
			return;
		}

		// Create graphics pipeline
		VkPipelineShaderStageCreateInfo shader_stages[2] = {
			{
				.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage  = VK_SHADER_STAGE_VERTEX_BIT,
				.module = fragment_shader->vertex_stage.shader,
				.pName  = "vs",
			},
			{
				.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
				.module = fragment_shader->pixel_stage.shader,
				.pName  = "ps",
			},
		};

		VkPipelineVertexInputStateCreateInfo vertex_input = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		};

		VkPipelineInputAssemblyStateCreateInfo input_assembly = {
			.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};

		VkPipelineViewportStateCreateInfo viewport_state = {
			.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.scissorCount  = 1,
		};

		VkPipelineRasterizationStateCreateInfo rasterizer = {
			.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode    = VK_CULL_MODE_NONE,
			.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
			.lineWidth   = 1.0f,
		};

		VkPipelineMultisampleStateCreateInfo multisampling = {
			.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};

		VkPipelineColorBlendAttachmentState color_blend_attachment = {
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
			.blendEnable    = VK_FALSE,
		};

		VkPipelineColorBlendStateCreateInfo color_blending = {
			.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments    = &color_blend_attachment,
		};

		VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynamic_state = {
			.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 2,
			.pDynamicStates    = dynamic_states,
		};

		VkGraphicsPipelineCreateInfo pipeline_info = {
			.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.stageCount          = 2,
			.pStages             = shader_stages,
			.pVertexInputState   = &vertex_input,
			.pInputAssemblyState = &input_assembly,
			.pViewportState      = &viewport_state,
			.pRasterizationState = &rasterizer,
			.pMultisampleState   = &multisampling,
			.pColorBlendState    = &color_blending,
			.pDynamicState       = &dynamic_state,
			.layout              = pipeline_layout,
			.renderPass          = render_pass,
			.subpass             = 0,
		};

		if (vkCreateGraphicsPipelines(_skr_vk.device, _skr_vk.pipeline_cache, 1, &pipeline_info, NULL, &pipeline) != VK_SUCCESS) {
			skr_log(skr_log_critical, "Failed to create pipeline for mipmap generation");
			vkDestroyPipelineLayout     (_skr_vk.device, pipeline_layout,   NULL);
			vkDestroyDescriptorSetLayout(_skr_vk.device, descriptor_layout, NULL);
			vkDestroyRenderPass         (_skr_vk.device, render_pass,       NULL);
			_skr_command_release(cmd);
			return;
		}
	}

	// Create a uniform buffer for mipgen parameters
	typedef struct {
		uint32_t src_size[2];
		uint32_t dst_size[2];
		uint32_t src_mip_level;
		uint32_t mip_max;
		uint32_t _pad[2];
	} mipgen_params_t;

	// Pre-populate parameters for all mip levels
	int32_t num_mips = mip_levels - 1;
	mipgen_params_t* all_params = calloc(num_mips, sizeof(mipgen_params_t));

	for (int32_t mip = 1; mip < mip_levels; mip++) {
		uint32_t mip_width  = tex->size.x >> mip;
		uint32_t mip_height = tex->size.y >> mip;
		if (mip_width  == 0) mip_width  = 1;
		if (mip_height == 0) mip_height = 1;

		uint32_t prev_mip_width  = tex->size.x >> (mip - 1);
		uint32_t prev_mip_height = tex->size.y >> (mip - 1);
		if (prev_mip_width  == 0) prev_mip_width  = 1;
		if (prev_mip_height == 0) prev_mip_height = 1;

		all_params[mip - 1].src_size[0] = prev_mip_width;
		all_params[mip - 1].src_size[1] = prev_mip_height;
		all_params[mip - 1].dst_size[0] = mip_width;
		all_params[mip - 1].dst_size[1] = mip_height;
		all_params[mip - 1].src_mip_level = mip - 1;
		all_params[mip - 1].mip_max = mip_levels;
	}

	// Create a buffer large enough for all mip levels
	skr_buffer_t params_buffer = skr_buffer_create(all_params, num_mips, sizeof(mipgen_params_t), skr_buffer_type_constant, skr_use_static);
	free(all_params);

	// Note: Per-mip resources (framebuffers, image views) will be added to the
	// command buffer's destroy list as we create them, so they'll be cleaned up
	// automatically when the command buffer fence signals

	// Mip 0 is already in SHADER_READ_ONLY_OPTIMAL from texture creation
	// No barrier needed for the first mip level
	VkImageMemoryBarrier barrier = {
		.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image               = tex->image,
		.subresourceRange    = {
			.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel   = 0,
			.levelCount     = 1,
			.baseArrayLayer = 0,
			.layerCount     = tex->layer_count,
		},
	};

	// Generate each mip level by rendering
	for (int32_t mip = 1; mip < mip_levels; mip++) {
		uint32_t mip_width  = tex->size.x >> mip;
		uint32_t mip_height = tex->size.y >> mip;
		if (mip_width  == 0) mip_width  = 1;
		if (mip_height == 0) mip_height = 1;

		uint32_t prev_mip_width  = tex->size.x >> (mip - 1);
		uint32_t prev_mip_height = tex->size.y >> (mip - 1);
		if (prev_mip_width  == 0) prev_mip_width  = 1;
		if (prev_mip_height == 0) prev_mip_height = 1;

		// Determine view type
		VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
		if (tex->flags & skr_tex_flags_cubemap) {
			view_type = VK_IMAGE_VIEW_TYPE_CUBE;
		} else if (tex->flags & skr_tex_flags_array) {
			view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		}

		// Create image view for this specific mip level
		VkImageView mip_view = VK_NULL_HANDLE;
		{
			VkImageViewCreateInfo view_info = {
				.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image      = tex->image,
				.viewType   = view_type,
				.format     = format,
				.subresourceRange = {
					.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel   = mip,
					.levelCount     = 1,
					.baseArrayLayer = 0,
					.layerCount     = tex->layer_count,
				},
			};
			if (vkCreateImageView(_skr_vk.device, &view_info, NULL, &mip_view) != VK_SUCCESS) {
				skr_log(skr_log_warning, "Failed to create image view for mip level");
				continue;
			}
			// Add to destroy list for cleanup when command buffer fence signals
			_skr_destroy_list_add_image_view(ctx.destroy_list, mip_view);
		}

		// Create framebuffer for this mip level
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		{
			VkFramebufferCreateInfo fb_info = {
				.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
				.renderPass      = render_pass,
				.attachmentCount = 1,
				.pAttachments    = &mip_view,
				.width           = mip_width,
				.height          = mip_height,
				.layers          = tex->layer_count,
			};
			if (vkCreateFramebuffer(_skr_vk.device, &fb_info, NULL, &framebuffer) != VK_SUCCESS) {
				skr_log(skr_log_warning, "Failed to create framebuffer for mip level");
				continue;
			}
			// Add to destroy list for cleanup when command buffer fence signals
			_skr_destroy_list_add_framebuffer(ctx.destroy_list, framebuffer);
		}

		// Transition current mip to color attachment
		barrier.srcAccessMask       = 0;
		barrier.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.subresourceRange.baseMipLevel = mip;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                     0, 0, NULL, 0, NULL, 1, &barrier);

		// Begin render pass
		VkRenderPassBeginInfo rp_begin = {
			.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass      = render_pass,
			.framebuffer     = framebuffer,
			.renderArea      = {{0, 0}, {mip_width, mip_height}},
			.clearValueCount = 0,
		};
		vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

		// Bind pipeline
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

		// Set viewport and scissor
		VkViewport viewport = {0, 0, (float)mip_width, (float)mip_height, 0.0f, 1.0f};
		VkRect2D   scissor  = {{0, 0}, {mip_width, mip_height}};
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor (cmd, 0, 1, &scissor);

		// Push descriptors (use offset for this specific mip level)
		VkDescriptorBufferInfo buffer_info = {
			.buffer = params_buffer.buffer,
			.offset = (mip - 1) * sizeof(mipgen_params_t),
			.range  = sizeof(mipgen_params_t),
		};

		// Create image view for the previous mip level we're sampling from
		VkImageView src_view = VK_NULL_HANDLE;
		{
			VkImageViewCreateInfo src_view_info = {
				.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image      = tex->image,
				.viewType   = view_type,
				.format     = format,
				.subresourceRange = {
					.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel   = mip - 1,
					.levelCount     = 1,
					.baseArrayLayer = 0,
					.layerCount     = tex->layer_count,
				},
			};
			if (vkCreateImageView(_skr_vk.device, &src_view_info, NULL, &src_view) != VK_SUCCESS) {
				skr_log(skr_log_warning, "Failed to create source mip image view");
				continue;
			}
			// Add to destroy list for cleanup when command buffer fence signals
			_skr_destroy_list_add_image_view(ctx.destroy_list, src_view);
		}

		VkDescriptorImageInfo image_info = {
			.sampler     = tex->sampler,
			.imageView   = src_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};

		VkWriteDescriptorSet writes[2] = { {
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding      = bind_globals.slot,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo     = &buffer_info,
			}, {
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding      = bind_source.slot,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo      = &image_info,
			},
		};
		// TODO TBH, this all needs to be re-written as a material! And re-use all the new binding code!!
		skr_log(skr_log_critical, "this all needs to be re-written as a material! And re-use all the new binding code!!");

		_skr_log_descriptor_writes(writes, &buffer_info, &image_info, 2, 1, 1);

		vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2, writes);

		// Draw fullscreen triangle (with instances for each layer/face)
		vkCmdDraw(cmd, 3, tex->layer_count, 0, 0);

		// End render pass
		vkCmdEndRenderPass(cmd);

		// Transition current mip to shader read for next iteration
		barrier.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.subresourceRange.baseMipLevel = mip;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                     0, 0, NULL, 0, NULL, 1, &barrier);
	}

	// Add the pipeline objects to the destroy list so they're cleaned up with the command buffer
	_skr_destroy_list_add_render_pass          (ctx.destroy_list, render_pass);
	_skr_destroy_list_add_pipeline             (ctx.destroy_list, pipeline);
	_skr_destroy_list_add_pipeline_layout      (ctx.destroy_list, pipeline_layout);
	_skr_destroy_list_add_descriptor_set_layout(ctx.destroy_list, descriptor_layout);
	skr_buffer_destroy(&params_buffer);
	
	// Submit command buffer
	// Note: framebuffers, image views, and pipeline objects will be automatically destroyed
	// when the command buffer fence signals (via the destroy list)
	_skr_command_release(cmd);

}

///////////////////////////////////////////////////////////////////////////////
// Sampler creation
///////////////////////////////////////////////////////////////////////////////

VkSampler _skr_sampler_create_vk(skr_tex_sampler_t settings) {
	VkFilter             filter      = _skr_to_vk_filter (settings.sample);
	VkSamplerAddressMode address     = _skr_to_vk_address(settings.address);
	VkCompareOp          compare_op  = _skr_to_vk_compare(settings.sample_compare);
	bool                 anisotropic = settings.sample == skr_tex_sample_anisotropic;

	VkSamplerCreateInfo sampler_info = {
		.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter               = filter,
		.minFilter               = filter,
		.mipmapMode              = filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU            = address,
		.addressModeV            = address,
		.addressModeW            = address,
		.anisotropyEnable        = anisotropic ? VK_TRUE : VK_FALSE,
		.maxAnisotropy           = anisotropic ? (float)settings.anisotropy : 1.0f,
		.compareEnable           = settings.sample_compare != skr_compare_none ? VK_TRUE : VK_FALSE,
		.compareOp               = compare_op,
		.minLod                  = 0.0f,
		.maxLod                  = VK_LOD_CLAMP_NONE,
		.borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
		.unnormalizedCoordinates = VK_FALSE,
	};

	VkSampler vk_sampler = VK_NULL_HANDLE;
	if (vkCreateSampler(_skr_vk.device, &sampler_info, NULL, &vk_sampler) != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to create sampler");
		return VK_NULL_HANDLE;
	}

	// Generate debug name based on sampler settings
	const char* filter_str = settings.sample  == skr_tex_sample_linear      ? "linear" :
	                         settings.sample  == skr_tex_sample_point       ? "point" :
	                         settings.sample  == skr_tex_sample_anisotropic ? "aniso" : "unk";
	const char* address_str = settings.address == skr_tex_address_wrap   ? "wrap" :
	                          settings.address == skr_tex_address_clamp  ? "clamp" :
	                          settings.address == skr_tex_address_mirror ? "mirror" : "unk";
	const char* compare_str = settings.sample_compare == skr_compare_none       ? "" :
	                          settings.sample_compare == skr_compare_less       ? "_less" :
	                          settings.sample_compare == skr_compare_less_or_eq ? "_lesseq" : "_cmp";

	char name[128];
	snprintf(name, sizeof(name), "sampler_%s_%s_%s", filter_str, address_str, compare_str);
	_skr_set_debug_name(VK_OBJECT_TYPE_SAMPLER, (uint64_t)vk_sampler, name);

	return vk_sampler;
}
