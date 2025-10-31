// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "sk_renderer.h"
#include "_sk_renderer.h"

#include "skr_vulkan.h"
#include "skr_conversions.h"
#include "skr_pipeline.h"
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

	VkResult vr = vkAllocateMemory(_skr_vk.device, &alloc_info, NULL, out_memory);
	SKR_VK_CHECK_RET(vr, "vkAllocateMemory", VK_NULL_HANDLE);

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

	VkResult vr = vkCreateBuffer(_skr_vk.device, &buffer_info, NULL, &result.buffer);
	SKR_VK_CHECK_RET(vr, "vkCreateBuffer", result);

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

	vr = vkAllocateMemory(_skr_vk.device, &alloc_info, NULL, &result.memory);
	if (vr != VK_SUCCESS) {
		SKR_VK_CHECK_NRET(vr, "vkAllocateMemory");
		vkDestroyBuffer(_skr_vk.device, result.buffer, NULL);
		return result;
	}

	vkBindBufferMemory(_skr_vk.device, result.buffer, result.memory, 0);

	vr = vkMapMemory(_skr_vk.device, result.memory, 0, size, 0, &result.mapped_data);
	if (vr != VK_SUCCESS) {
		SKR_VK_CHECK_NRET(vr, "vkMapMemory");
		vkFreeMemory(_skr_vk.device, result.memory, NULL);
		vkDestroyBuffer(_skr_vk.device, result.buffer, NULL);
		return result;
	}

	result.valid = true;
	return result;
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

skr_err_ skr_tex_create(skr_tex_fmt_ format, skr_tex_flags_ flags, skr_tex_sampler_t sampler,
                         skr_vec3i_t size, int32_t multisample, int32_t mip_count, const void* opt_tex_data, skr_tex_t* out_tex) {
	if (!out_tex) return skr_err_invalid_parameter;

	// Zero out immediately
	memset(out_tex, 0, sizeof(skr_tex_t));

	// Validate parameters
	if (size.x <= 0 || size.y <= 0 || size.z <= 0) {
		skr_log(skr_log_warning, "Invalid texture size");
		return skr_err_invalid_parameter;
	}

	out_tex->size             = size;
	out_tex->flags            = flags;
	out_tex->sampler_settings = sampler;
	out_tex->format           = format;

	VkFormat vk_format = _skr_to_vk_tex_fmt(format);
	if (vk_format == VK_FORMAT_UNDEFINED) {
		return skr_err_unsupported;
	}

	// Determine image type, layer count, and normalize counts
	VkImageType image_type;
	if (out_tex->flags & skr_tex_flags_3d) {
		image_type  = VK_IMAGE_TYPE_3D;
		out_tex->layer_count = 1;
	} else if (out_tex->flags & skr_tex_flags_array) {
		image_type  = VK_IMAGE_TYPE_2D;
		out_tex->layer_count = size.z;
	} else if (out_tex->flags & skr_tex_flags_cubemap) {
		image_type  = VK_IMAGE_TYPE_2D;
		out_tex->layer_count = 6;
	} else {
		image_type = (size.z > 1) ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_2D;
		out_tex->layer_count = 1;
	}

	// Normalize mip count (default to 1 if not specified or zero)
	out_tex->mip_levels = mip_count <= 0 ? 1 : mip_count;

	// Normalize sample count
	out_tex->samples = (multisample > 1) ? (VkSampleCountFlagBits)multisample : VK_SAMPLE_COUNT_1_BIT;

	// Determine usage flags
	VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT; // Always allow sampling for readable textures
	if (out_tex->flags & skr_tex_flags_readable) {
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	// Check if this is a depth format
	bool is_depth = (format == skr_tex_fmt_depth16 || format == skr_tex_fmt_depth32 || format == skr_tex_fmt_depth32s8 || format == skr_tex_fmt_depth24s8 || format == skr_tex_fmt_depth16s8);

	// Determine aspect mask based on format
	out_tex->aspect_mask  = 0;
	if (is_depth)                           { out_tex->aspect_mask  |= VK_IMAGE_ASPECT_DEPTH_BIT;   }
	if (_skr_format_has_stencil(vk_format)) { out_tex->aspect_mask  |= VK_IMAGE_ASPECT_STENCIL_BIT; }
	if (out_tex->aspect_mask == 0)          { out_tex->aspect_mask   = VK_IMAGE_ASPECT_COLOR_BIT;   }


	// For MSAA attachments, add transient bit for in-tile resolve optimization
	// But only if the texture is NOT readable (transient means no memory backing)
	bool is_msaa_attachment = out_tex->samples > VK_SAMPLE_COUNT_1_BIT && (out_tex->flags & skr_tex_flags_writeable) && !(out_tex->flags & skr_tex_flags_readable);

	if (out_tex->flags & skr_tex_flags_writeable) {
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
	if (out_tex->flags & skr_tex_flags_compute) {
		usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	}

	// For mipmap generation
	if (out_tex->flags & skr_tex_flags_gen_mips) {
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
		if (out_tex->mip_levels == 1) {
			int32_t max_dim = size.x > size.y ? size.x : size.y;
			out_tex->mip_levels = (int32_t)floor(log2(max_dim)) + 1;
		}
	}

	// Create image
	VkImageCreateInfo image_info = {
		.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType     = image_type,
		.format        = vk_format,
		.extent        = { .width = size.x, .height = size.y, .depth = (out_tex->flags & skr_tex_flags_3d) ? size.z : 1 },
		.mipLevels     = out_tex->mip_levels,
		.arrayLayers   = out_tex->layer_count,
		.samples       = out_tex->samples,
		.tiling        = VK_IMAGE_TILING_OPTIMAL,
		.usage         = usage,
		.sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.flags         = (out_tex->flags & skr_tex_flags_cubemap) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
	};

	VkResult vr = vkCreateImage(_skr_vk.device, &image_info, NULL, &out_tex->image);
	if (vr != VK_SUCCESS) {
		skr_log(skr_log_critical, "vkCreateImage failed");
		return skr_err_device_error;
	}

	// Allocate memory using helper
	if (_skr_allocate_image_memory(out_tex->image, is_msaa_attachment, &out_tex->memory) == VK_NULL_HANDLE) {
		skr_log(skr_log_critical, "Failed to allocate texture memory");
		vkDestroyImage(_skr_vk.device, out_tex->image, NULL);
		memset(out_tex, 0, sizeof(skr_tex_t));
		return skr_err_out_of_memory;
	}

	vkBindImageMemory(_skr_vk.device, out_tex->image, out_tex->memory, 0);

	// Upload texture data if provided (or just transition to shader read layout)
	if (opt_tex_data) {
		// Calculate data size and create staging buffer
		uint32_t           pixel_size = _skr_tex_fmt_to_size(format);
		VkDeviceSize       data_size  = size.x * size.y * size.z * pixel_size;
		staging_buffer_t   staging    = _skr_create_staging_buffer(data_size);

		if (!staging.valid) {
			skr_log(skr_log_critical, "Failed to create staging buffer for texture upload");
			vkFreeMemory(_skr_vk.device, out_tex->memory, NULL);
			vkDestroyImage(_skr_vk.device, out_tex->image, NULL);
			memset(out_tex, 0, sizeof(skr_tex_t));
			return skr_err_out_of_memory;
		}

		// Copy texture data to staging buffer
		memcpy(staging.mapped_data, opt_tex_data, data_size);

		// Create command buffer and upload (async with deferred cleanup)
		_skr_command_context_t ctx = _skr_command_acquire();

		// Transition: UNDEFINED -> TRANSFER_DST (using automatic system)
		_skr_tex_transition(ctx.cmd, out_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

		// Copy buffer to image (all layers at once for arrays/cubemaps)
		vkCmdCopyBufferToImage(ctx.cmd, staging.buffer, out_tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkBufferImageCopy){
			.bufferOffset      = 0,
			.bufferRowLength   = 0,
			.bufferImageHeight = 0,
			.imageSubresource  = {
				.aspectMask     = out_tex->aspect_mask,
				.mipLevel       = 0,
				.baseArrayLayer = 0,
				.layerCount     = out_tex->layer_count,
			},
			.imageOffset = {0, 0, 0},
			.imageExtent = {size.x, size.y, (out_tex->flags & skr_tex_flags_3d) ? size.z : 1},
		});

		// Transition: TRANSFER_DST -> SHADER_READ_ONLY (using automatic system)
		// Include compute shader stage if texture has compute flag
		VkPipelineStageFlags shader_stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		if (out_tex->flags & skr_tex_flags_compute) {
			shader_stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		}
		_skr_tex_transition_for_shader_read(ctx.cmd, out_tex, shader_stages);

		// Defer destruction of staging resources until GPU completes
		_skr_command_destroy_buffer(ctx.destroy_list, staging.buffer);
		_skr_command_destroy_memory(ctx.destroy_list, staging.memory);
		_skr_command_release(ctx.cmd);
	} else if (!is_msaa_attachment && !(out_tex->flags & skr_tex_flags_writeable)) {
		// No data provided, transition to appropriate layout for read-only textures
		// Skip for transient MSAA attachments - they don't need initial layout transition
		// Skip for writeable textures - let the first render pass handle the transition

		_skr_command_context_t ctx = _skr_command_acquire();
		// Use automatic transition system - handles storage vs regular textures
		if (out_tex->flags & skr_tex_flags_compute) { _skr_tex_transition_for_storage    (ctx.cmd, out_tex); }
		else                                        { _skr_tex_transition_for_shader_read(ctx.cmd, out_tex, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT); }
		_skr_command_release(ctx.cmd);
	}

	// Create image view
	VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
	if      (out_tex->flags & skr_tex_flags_3d     ) view_type = VK_IMAGE_VIEW_TYPE_3D;
	else if (out_tex->flags & skr_tex_flags_cubemap) view_type = VK_IMAGE_VIEW_TYPE_CUBE;
	else if (out_tex->flags & skr_tex_flags_array  ) view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;

	VkImageViewCreateInfo view_info = {
		.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image    = out_tex->image,
		.viewType = view_type,
		.format   = vk_format,
		.subresourceRange = {
			.aspectMask     = out_tex->aspect_mask,
			.baseMipLevel   = 0,
			.levelCount     = image_info.mipLevels,
			.baseArrayLayer = 0,
			.layerCount     = image_info.arrayLayers,
		},
	};

	vr = vkCreateImageView(_skr_vk.device, &view_info, NULL, &out_tex->view);
	if (vr != VK_SUCCESS) {
		skr_log(skr_log_critical, "vkCreateImageView failed");
		vkFreeMemory  (_skr_vk.device, out_tex->memory, NULL);
		vkDestroyImage(_skr_vk.device, out_tex->image,  NULL);
		memset(out_tex, 0, sizeof(skr_tex_t));
		return skr_err_device_error;
	}

	// Store texture properties
	out_tex->sampler              = _skr_sampler_create_vk(sampler);

	// Initialize layout tracking
	out_tex->current_layout       = VK_IMAGE_LAYOUT_UNDEFINED;
	out_tex->current_queue_family = _skr_vk.graphics_queue_family;
	out_tex->first_use            = true;
	// Transient discard optimization for non-readable depth/MSAA (tile GPU optimization)
	out_tex->is_transient_discard = (is_msaa_attachment || (is_depth && !(flags & skr_tex_flags_readable)));

	return skr_err_success;
}

void skr_tex_destroy(skr_tex_t* tex) {
	if (!tex) return;

	_skr_command_destroy_framebuffer(NULL, tex->framebuffer);
	_skr_command_destroy_framebuffer(NULL, tex->framebuffer_depth);
	_skr_command_destroy_sampler    (NULL, tex->sampler);
	_skr_command_destroy_image_view (NULL, tex->view);
	_skr_command_destroy_image      (NULL, tex->image);
	_skr_command_destroy_memory     (NULL, tex->memory);
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

	_skr_command_context_t ctx = _skr_command_acquire();

	// Transition mip 0 to TRANSFER_SRC_OPTIMAL (automatic system tracks current layout)
	_skr_tex_transition(ctx.cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);

	// Generate each mip level by blitting from the previous level
	int32_t mip_width  = tex->size.x;
	int32_t mip_height = tex->size.y;

	for (int32_t i = 1; i < mip_levels; i++) {
		int32_t next_mip_width  = mip_width  > 1 ? mip_width  / 2 : 1;
		int32_t next_mip_height = mip_height > 1 ? mip_height / 2 : 1;

		// Transition mip i from UNDEFINED to TRANSFER_DST_OPTIMAL
		_skr_transition_image_layout(ctx.cmd, tex->image, tex->aspect_mask, i, 1, tex->layer_count,
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

		vkCmdBlitImage(ctx.cmd,
			tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, filter_mode);

		// Transition mip i from TRANSFER_DST to TRANSFER_SRC for next iteration
		_skr_transition_image_layout(ctx.cmd, tex->image, tex->aspect_mask, i, 1, tex->layer_count,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

		mip_width  = next_mip_width;
		mip_height = next_mip_height;
	}

	// Transition back to shader read layout (automatic system handles this)
	_skr_tex_transition_for_shader_read(ctx.cmd, tex, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	_skr_command_release(ctx.cmd);
}

static void _skr_tex_generate_mips_render(skr_tex_t* tex, int32_t mip_levels, const skr_shader_t* fragment_shader) {
	if (!skr_shader_is_valid(fragment_shader)) {
		skr_log(skr_log_warning, "Invalid fragment shader provided for mipmap generation");
		return;
	}

	// Validate that shader has required 'src_tex' binding
	skr_bind_t bind_source = skr_shader_get_bind(fragment_shader, "src_tex");
	if ((bind_source.stage_bits & skr_stage_pixel) == 0) {
		skr_log(skr_log_warning, "Mip shader missing 'src_tex' binding");
		return;
	}

	// Create material from shader (handles pipeline registration automatically)
	skr_material_t material;
	skr_material_create((skr_material_info_t){
		.shader      = fragment_shader,
		.cull        = skr_cull_none,
		.depth_test  = skr_compare_always,
		.write_mask  = skr_write_rgba,
	}, &material);
	if (!skr_material_is_valid(&material)) {
		skr_log(skr_log_warning, "Failed to create material for mipmap generation");
		return;
	}

	// Register render pass format with pipeline system (cached for reuse)
	VkFormat format = _skr_to_vk_tex_fmt(tex->format);
	skr_pipeline_renderpass_key_t rp_key = {
		.color_format   = format,
		.depth_format   = VK_FORMAT_UNDEFINED,
		.resolve_format = VK_FORMAT_UNDEFINED,
		.samples        = VK_SAMPLE_COUNT_1_BIT,
		.depth_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.color_load_op  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,  // Full blit
	};
	int32_t renderpass_idx = _skr_pipeline_register_renderpass(&rp_key);
	int32_t vert_idx       = _skr_pipeline_register_vertformat((skr_vert_type_t){});

	// Get cached render pass
	VkRenderPass render_pass = _skr_pipeline_get_renderpass(renderpass_idx);
	if (render_pass == VK_NULL_HANDLE) {
		skr_material_destroy(&material);
		return;
	}

	// Acquire command buffer context
	_skr_command_context_t ctx = _skr_command_acquire();
	if (!ctx.cmd) {
		skr_log(skr_log_warning, "Failed to acquire command buffer for mipmap generation");
		skr_material_destroy(&material);
		return;
	}

	// Determine view type for layer rendering
	VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
	if (tex->flags & skr_tex_flags_cubemap) {
		view_type = VK_IMAGE_VIEW_TYPE_CUBE;
	} else if (tex->flags & skr_tex_flags_array) {
		view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	}

	// Get cached pipeline (lazy creation via 3D cache: material × renderpass × vertformat)
	VkPipeline pipeline = _skr_pipeline_get(material.pipeline_material_idx, renderpass_idx, vert_idx);
	if (pipeline == VK_NULL_HANDLE) {
		skr_log(skr_log_warning, "Failed to get pipeline for mipmap generation");
		_skr_command_release(ctx.cmd);
		skr_material_destroy(&material);
		return;
	}

	// Pre-populate parameter buffers for all mip levels
	// Use material API to set values (handles different $Global layouts per shader)
	int32_t      num_mips      = mip_levels - 1;
	uint8_t*     all_params    = NULL;
	skr_buffer_t params_buffer = {0};

	if (material.param_buffer_size > 0) {
		all_params = calloc(num_mips, material.param_buffer_size);

		for (int32_t mip = 1; mip < mip_levels; mip++) {
			uint32_t mip_width  = tex->size.x >> mip;
			uint32_t mip_height = tex->size.y >> mip;
			if (mip_width  == 0) mip_width  = 1;
			if (mip_height == 0) mip_height = 1;

			uint32_t prev_mip_width  = tex->size.x >> (mip - 1);
			uint32_t prev_mip_height = tex->size.y >> (mip - 1);
			if (prev_mip_width  == 0) prev_mip_width  = 1;
			if (prev_mip_height == 0) prev_mip_height = 1;

			// Use material API to populate values (handles different shader layouts)
			skr_material_set_vec2i(&material, "src_size", (skr_vec2i_t){prev_mip_width, prev_mip_height});
			skr_material_set_vec2i(&material, "dst_size", (skr_vec2i_t){mip_width, mip_height});
			skr_material_set_uint (&material, "src_mip_level", mip - 1);
			skr_material_set_uint (&material, "mip_max",       mip_levels);

			// Copy material's parameter buffer for this mip (preserves other values)
			memcpy(all_params + (mip - 1) * material.param_buffer_size,
			       material.param_buffer,
			       material.param_buffer_size);
		}

		// Create GPU buffer with all mip parameters
		skr_buffer_create(all_params, num_mips, material.param_buffer_size, skr_buffer_type_constant, skr_use_static, &params_buffer);
		free(all_params);
	}

	// Generate each mip level by rendering from previous mip
	for (int32_t mip = 1; mip < mip_levels; mip++) {
		uint32_t mip_width  = tex->size.x >> mip;
		uint32_t mip_height = tex->size.y >> mip;
		if (mip_width  == 0) mip_width  = 1;
		if (mip_height == 0) mip_height = 1;

		// Create image view for this mip level (target)
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
				skr_log(skr_log_warning, "Failed to create target mip image view");
				continue;
			}
			_skr_command_destroy_image_view(ctx.destroy_list, mip_view);
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
			_skr_command_destroy_framebuffer(ctx.destroy_list, framebuffer);
		}

		// Create image view for the previous mip level (source)
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
			_skr_command_destroy_image_view(ctx.destroy_list, src_view);
		}

		// Transition current mip to color attachment (automatic tracking handles UNDEFINED vs previous layout)
		VkImageMemoryBarrier barrier = {
			.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask       = 0,
			.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image               = tex->image,
			.subresourceRange    = {
				.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel   = mip,
				.levelCount     = 1,
				.baseArrayLayer = 0,
				.layerCount     = tex->layer_count,
			},
		};
		vkCmdPipelineBarrier(ctx.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                     0, 0, NULL, 0, NULL, 1, &barrier);

		// Begin render pass
		vkCmdBeginRenderPass(ctx.cmd, &(VkRenderPassBeginInfo){
			.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass      = render_pass,
			.framebuffer     = framebuffer,
			.renderArea      = {{0, 0}, {mip_width, mip_height}},
			.clearValueCount = 0,
		}, VK_SUBPASS_CONTENTS_INLINE);

		// Bind pipeline
		vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

		// Set viewport and scissor
		VkViewport viewport = {0, 0, (float)mip_width, (float)mip_height, 0.0f, 1.0f};
		VkRect2D   scissor  = {{0, 0}, {mip_width, mip_height}};
		vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
		vkCmdSetScissor (ctx.cmd, 0, 1, &scissor);

		// Build descriptor writes using material system
		VkWriteDescriptorSet   writes      [32];
		VkDescriptorBufferInfo buffer_infos[16];
		VkDescriptorImageInfo  image_infos [16];
		uint32_t write_ct  = 0;
		uint32_t buffer_ct = 0;
		uint32_t image_ct  = 0;

		// Handle material parameters if present ($Global buffer with per-mip offset)
		if (skr_buffer_is_valid(&params_buffer)) {
			buffer_infos[buffer_ct] = (VkDescriptorBufferInfo){
				.buffer = params_buffer.buffer,
				.offset = (mip - 1) * material.param_buffer_size,  // Offset for this mip
				.range  = material.param_buffer_size,
			};
			writes[write_ct++] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding      = SKR_BIND_SHIFT_BUFFER + SKR_BIND_MATERIAL,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo     = &buffer_infos[buffer_ct++],
			};
		}

		// Manually add source texture binding (since we create it per-mip)
		image_infos[image_ct] = (VkDescriptorImageInfo){
			.sampler     = tex->sampler,
			.imageView   = src_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		writes[write_ct++] = (VkWriteDescriptorSet){
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstBinding      = bind_source.slot,
			.descriptorCount = 1,
			.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo      = &image_infos[image_ct++],
		};

		// Add any other material bindings (textures/buffers, including globals)
		const int32_t ignore_slots[] = {
			SKR_BIND_SHIFT_BUFFER + SKR_BIND_MATERIAL,  // Already handled above
			bind_source.slot                             // Source texture (per-mip, handled above)
		};
		_skr_material_add_writes(
			material.binds, material.bind_count,
			ignore_slots, sizeof(ignore_slots)/sizeof(ignore_slots[0]),
			writes,       sizeof(writes      )/sizeof(writes      [0]),
			buffer_infos, sizeof(buffer_infos)/sizeof(buffer_infos[0]),
			image_infos,  sizeof(image_infos )/sizeof(image_infos [0]),
			&write_ct, &buffer_ct, &image_ct
		);

		// Push descriptors and draw
		if (write_ct > 0) {
			vkCmdPushDescriptorSetKHR(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
			                          _skr_pipeline_get_layout(material.pipeline_material_idx),
			                          0, write_ct, writes);
		}

		// Draw fullscreen triangle (with instances for each layer/face)
		vkCmdDraw(ctx.cmd, 3, tex->layer_count, 0, 0);

		// End render pass
		vkCmdEndRenderPass(ctx.cmd);

		// Transition current mip to shader read for next iteration
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(ctx.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                     0, 0, NULL, 0, NULL, 1, &barrier);
	}

	skr_buffer_destroy(&params_buffer);
	skr_material_destroy(&material);

	_skr_command_release(ctx.cmd);
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
	VkResult vr = vkCreateSampler(_skr_vk.device, &sampler_info, NULL, &vk_sampler);
	SKR_VK_CHECK_RET(vr, "vkCreateSampler", VK_NULL_HANDLE);

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
