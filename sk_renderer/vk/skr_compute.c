// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "../include/sk_renderer.h"
#include "skr_vulkan.h"
#include "_sk_renderer.h"
#include "../skr_log.h"

#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Forward declarations
///////////////////////////////////////////////////////////////////////////////

static VkDescriptorSet _skr_compute_allocate_and_update_descriptor_set(skr_compute_t* compute);

///////////////////////////////////////////////////////////////////////////////

skr_compute_t skr_compute_create(const skr_shader_t* shader) {
	skr_compute_t compute = {0};

	if (!shader || !skr_shader_is_valid(shader) || shader->compute_stage.shader == VK_NULL_HANDLE) {
		skr_log(skr_log_critical, "Invalid shader or no compute stage");
		return compute;
	}

	compute.shader = shader;

	// Create descriptor set layout
	if (shader->meta && (shader->meta->buffer_count > 0 || shader->meta->resource_count > 0)) {
		VkDescriptorSetLayoutBinding bindings[32];
		uint32_t binding_count = 0;

		// Add buffer bindings
		for (uint32_t i = 0; i < shader->meta->buffer_count; i++) {
			VkDescriptorType desc_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			if (shader->meta->buffers[i].bind.register_type == skr_register_readwrite) {
				desc_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			}

			bindings[binding_count++] = (VkDescriptorSetLayoutBinding){
				.binding            = shader->meta->buffers[i].bind.slot,
				.descriptorType     = desc_type,
				.descriptorCount    = 1,
				.stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
				.pImmutableSamplers = NULL,
			};
		}

		// Add resource bindings (textures, storage buffers, and storage images)
		for (uint32_t i = 0; i < shader->meta->resource_count; i++) {
			VkDescriptorType desc_type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

			skr_register_ reg_type = shader->meta->resources[i].bind.register_type;
			if (reg_type == skr_register_readwrite_tex) {
				desc_type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			} else if (reg_type == skr_register_texture) {
				desc_type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			} else if (reg_type == skr_register_readwrite || reg_type == skr_register_read_buffer) {
				// Both StructuredBuffer and RWStructuredBuffer map to storage buffers in Vulkan
				desc_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			}

			bindings[binding_count++] = (VkDescriptorSetLayoutBinding){
				.binding            = shader->meta->resources[i].bind.slot,
				.descriptorType     = desc_type,
				.descriptorCount    = 1,
				.stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
				.pImmutableSamplers = NULL,
			};
		}

		VkDescriptorSetLayoutCreateInfo layout_info = {
			.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
			.bindingCount = binding_count,
			.pBindings    = bindings,
		};

		if (vkCreateDescriptorSetLayout(_skr_vk.device, &layout_info, NULL, &compute.descriptor_layout) != VK_SUCCESS) {
			skr_log(skr_log_critical, "Failed to create compute descriptor set layout");
			skr_compute_destroy(&compute);
			return compute;
		}
	}

	// Create pipeline layout
	VkPipelineLayoutCreateInfo pipeline_layout_info = {
		.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = compute.descriptor_layout != VK_NULL_HANDLE ? 1 : 0,
		.pSetLayouts    = compute.descriptor_layout != VK_NULL_HANDLE ? &compute.descriptor_layout : NULL,
	};

	if (vkCreatePipelineLayout(_skr_vk.device, &pipeline_layout_info, NULL, &compute.layout) != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to create compute pipeline layout");
		skr_compute_destroy(&compute);
		return compute;
	}

	// Create compute pipeline
	VkComputePipelineCreateInfo pipeline_info = {
		.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage  = (VkPipelineShaderStageCreateInfo){
			.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage  = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = shader->compute_stage.shader,
			.pName  = "cs",
		},
		.layout = compute.layout,
	};

	if (vkCreateComputePipelines(_skr_vk.device, _skr_vk.pipeline_cache, 1, &pipeline_info, NULL, &compute.pipeline) != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to create compute pipeline");
		skr_compute_destroy(&compute);
		return compute;
	}

	return compute;
}

bool skr_compute_is_valid(const skr_compute_t* compute) {
	return compute && compute->pipeline != VK_NULL_HANDLE;
}

skr_bind_t skr_compute_get_bind(const skr_compute_t* compute, const char* bind_name) {
	if (!compute || !compute->shader || !compute->shader->meta) {
		skr_bind_t empty = {0};
		return empty;
	}
	return sksc_shader_meta_get_bind(compute->shader->meta, bind_name);
}

void skr_compute_destroy(skr_compute_t* compute) {
	if (!compute) return;

	_skr_command_context_t ctx;
	if (_skr_command_try_get_active(&ctx)) {
		// Deferred deletion: queue resources to command's destroy list
		if (compute->pipeline          != VK_NULL_HANDLE) _skr_destroy_list_add_pipeline             (ctx.destroy_list, compute->pipeline);
		if (compute->layout            != VK_NULL_HANDLE) _skr_destroy_list_add_pipeline_layout      (ctx.destroy_list, compute->layout);
		if (compute->descriptor_layout != VK_NULL_HANDLE) _skr_destroy_list_add_descriptor_set_layout(ctx.destroy_list, compute->descriptor_layout);
	} else {
		// No active command: destroy immediately
		if (compute->pipeline          != VK_NULL_HANDLE) vkDestroyPipeline            (_skr_vk.device, compute->pipeline,          NULL);
		if (compute->layout            != VK_NULL_HANDLE) vkDestroyPipelineLayout      (_skr_vk.device, compute->layout,            NULL);
		if (compute->descriptor_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(_skr_vk.device, compute->descriptor_layout, NULL);
	}

	// Note: shader is not owned by compute, so we don't destroy it

	memset(compute, 0, sizeof(skr_compute_t));
}

void skr_compute_set_buffer(skr_compute_t* compute, int32_t bind, skr_buffer_t* buffer) {
	if (!compute || bind < 0 || bind >= 16) return;
	compute->buffers[bind] = buffer;
	// Binding is staged - will be applied on next skr_compute_execute()
}

void skr_compute_set_tex(skr_compute_t* compute, int32_t bind, skr_tex_t* texture) {
	if (!compute || bind < 0 || bind >= 16) return;
	compute->textures[bind] = texture;
	// Binding is staged - will be applied on next skr_compute_execute()
}

// Build descriptor writes for push descriptors (no dstSet needed)
static uint32_t _skr_compute_build_descriptor_writes(skr_compute_t* compute,
                                                       VkWriteDescriptorSet* writes,
                                                       VkDescriptorBufferInfo* buffer_infos,
                                                       VkDescriptorImageInfo* image_infos) {
	uint32_t write_count = 0;
	uint32_t buffer_info_count = 0;
	uint32_t image_info_count = 0;

	if (!compute->shader || !compute->shader->meta) {
		return 0;
	}

	// Build buffer descriptors
	for (uint32_t i = 0; i < compute->shader->meta->buffer_count; i++) {
		int32_t slot = compute->shader->meta->buffers[i].bind.slot;
		if (slot >= 0 && slot < 16 && compute->buffers[slot]) {
			VkDescriptorType desc_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			if (compute->shader->meta->buffers[i].bind.register_type == skr_register_readwrite) {
				desc_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			}

			buffer_infos[buffer_info_count] = (VkDescriptorBufferInfo){
				.buffer = compute->buffers[slot]->buffer,
				.offset = 0,
				.range  = compute->buffers[slot]->size,
			};

			writes[write_count++] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet          = VK_NULL_HANDLE,  // Not used with push descriptors
				.dstBinding      = slot,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType  = desc_type,
				.pBufferInfo     = &buffer_infos[buffer_info_count++],
			};
		}
	}

	// Build resource descriptors (textures and storage buffers from StructuredBuffers)
	for (uint32_t i = 0; i < compute->shader->meta->resource_count; i++) {
		int32_t slot = compute->shader->meta->resources[i].bind.slot;
		skr_register_ reg_type = compute->shader->meta->resources[i].bind.register_type;

		// Handle storage buffers (StructuredBuffer is skr_register_read_buffer, RWStructuredBuffer is skr_register_readwrite)
		if ((reg_type == skr_register_read_buffer || reg_type == skr_register_readwrite) &&
		    slot >= 0 && slot < 16 && compute->buffers[slot]) {
			buffer_infos[buffer_info_count] = (VkDescriptorBufferInfo){
				.buffer = compute->buffers[slot]->buffer,
				.offset = 0,
				.range  = compute->buffers[slot]->size,
			};

			writes[write_count++] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet          = VK_NULL_HANDLE,  // Not used with push descriptors
				.dstBinding      = slot,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo     = &buffer_infos[buffer_info_count++],
			};
		}
		// Handle textures and storage images
		else if (slot >= 0 && slot < 16 && compute->textures[slot]) {
			VkDescriptorType desc_type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			if (reg_type == skr_register_readwrite_tex) {
				desc_type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				layout = VK_IMAGE_LAYOUT_GENERAL;
			} else if (compute->textures[slot]->flags & skr_tex_flags_compute) {
				// Textures created with compute flag are in GENERAL layout
				layout = VK_IMAGE_LAYOUT_GENERAL;
			}

			image_infos[image_info_count] = (VkDescriptorImageInfo){
				.sampler     = compute->textures[slot]->sampler,
				.imageView   = compute->textures[slot]->view,
				.imageLayout = layout,
			};

			writes[write_count++] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet          = VK_NULL_HANDLE,  // Not used with push descriptors
				.dstBinding      = slot,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType  = desc_type,
				.pImageInfo      = &image_infos[image_info_count++],
			};
		}
	}

	return write_count;
}

void skr_compute_execute(skr_compute_t* compute, uint32_t x, uint32_t y, uint32_t z) {
	if (!skr_compute_is_valid(compute)) return;

	VkCommandBuffer cmd = _skr_command_acquire().cmd;
	if (!cmd) {
		skr_log(skr_log_warning, "skr_compute_execute failed to acquire command buffer");
		return;
	}

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute->pipeline);

	// Transition all bound textures to appropriate layouts before dispatch
	const sksc_shader_meta_t* meta = compute->shader->meta;
	for (uint32_t i = 0; i < meta->resource_count; i++) {
		int32_t       slot     = meta->resources[i].bind.slot;
		skr_register_ reg_type = meta->resources[i].bind.register_type;

		if (slot >= 0 && slot < 16 && compute->textures[slot]) {
			if       (reg_type == skr_register_readwrite_tex) {_skr_tex_transition_for_storage    (cmd, compute->textures[slot]); }
			 else if (reg_type == skr_register_texture)       {_skr_tex_transition_for_shader_read(cmd, compute->textures[slot], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); }
		}
	}

	// Push descriptors directly (no allocation/pooling needed)
	if (compute->descriptor_layout) {
		VkWriteDescriptorSet   writes[32];
		VkDescriptorBufferInfo buffer_infos[16];
		VkDescriptorImageInfo  image_infos[16];

		uint32_t write_count = _skr_compute_build_descriptor_writes(compute, writes, buffer_infos, image_infos);
		if (write_count > 0) {
			vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute->layout, 0, write_count, writes);
		}
	}

	vkCmdDispatch(cmd, x, y, z);

	// Add memory barrier for storage resources to ensure writes are visible to next operation
	// This now includes both compute→compute and compute→fragment transitions
	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,  // Fixed: include fragment shader
		0, 1, &(VkMemoryBarrier){
			.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		}, 0, NULL, 0, NULL);

	_skr_command_release(cmd);
}

void skr_compute_execute_indirect(skr_compute_t* compute, skr_buffer_t* indirect_args) {
	if (!skr_compute_is_valid(compute) || !indirect_args) return;

	VkCommandBuffer cmd = _skr_command_acquire().cmd;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute->pipeline);

	// Push descriptors directly (no allocation/pooling needed)
	if (compute->descriptor_layout) {
		VkWriteDescriptorSet   writes[32];
		VkDescriptorBufferInfo buffer_infos[16];
		VkDescriptorImageInfo  image_infos[16];

		uint32_t write_count = _skr_compute_build_descriptor_writes(compute, writes, buffer_infos, image_infos);
		if (write_count > 0) {
			vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute->layout, 0, write_count, writes);
		}
	}

	vkCmdDispatchIndirect(cmd, indirect_args->buffer, 0);
	_skr_command_release(cmd);
}
