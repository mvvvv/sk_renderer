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

skr_err_ skr_compute_create(const skr_shader_t* shader, skr_compute_t* out_compute) {
	if (!out_compute) return skr_err_invalid_parameter;

	// Zero out immediately
	memset(out_compute, 0, sizeof(skr_compute_t));

	if (!shader || !skr_shader_is_valid(shader) || shader->compute_stage.shader == VK_NULL_HANDLE) {
		skr_log(skr_log_critical, "Invalid shader or no compute stage");
		return skr_err_invalid_parameter;
	}

	out_compute->shader = shader;

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
			.flags        = _skr_vk.has_push_descriptors ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
			.bindingCount = binding_count,
			.pBindings    = bindings,
		};

		VkResult vr = vkCreateDescriptorSetLayout(_skr_vk.device, &layout_info, NULL, &out_compute->descriptor_layout);
		SKR_VK_CHECK_RET(vr, "vkCreateDescriptorSetLayout", skr_err_device_error);
	}

	// Create pipeline layout
	VkPipelineLayoutCreateInfo pipeline_layout_info = {
		.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = out_compute->descriptor_layout != VK_NULL_HANDLE ? 1 : 0,
		.pSetLayouts    = out_compute->descriptor_layout != VK_NULL_HANDLE ? &out_compute->descriptor_layout : NULL,
	};

	VkResult vr = vkCreatePipelineLayout(_skr_vk.device, &pipeline_layout_info, NULL, &out_compute->layout);
	if (vr != VK_SUCCESS) {
		SKR_VK_CHECK_NRET(vr, "vkCreatePipelineLayout");
		if (out_compute->descriptor_layout) {
			vkDestroyDescriptorSetLayout(_skr_vk.device, out_compute->descriptor_layout, NULL);
		}
		memset(out_compute, 0, sizeof(skr_compute_t));
		return skr_err_device_error;
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
		.layout = out_compute->layout,
	};

	vr = vkCreateComputePipelines(_skr_vk.device, _skr_vk.pipeline_cache, 1, &pipeline_info, NULL, &out_compute->pipeline);
	if (vr != VK_SUCCESS) {
		SKR_VK_CHECK_NRET(vr, "vkCreateComputePipelines");
		vkDestroyPipelineLayout(_skr_vk.device, out_compute->layout, NULL);
		if (out_compute->descriptor_layout) {
			vkDestroyDescriptorSetLayout(_skr_vk.device, out_compute->descriptor_layout, NULL);
		}
		memset(out_compute, 0, sizeof(skr_compute_t));
		return skr_err_device_error;
	}

	// Allocate memory for our resource binds
	out_compute->bind_count = shader->meta->resource_count + shader->meta->buffer_count;
	out_compute->binds      = (skr_material_bind_t*)calloc(out_compute->bind_count, sizeof(skr_material_bind_t));
	for (uint32_t i = 0; i < shader->meta->buffer_count;   i++) out_compute->binds[i                           ].bind = shader->meta->buffers  [i].bind;
	for (uint32_t i = 0; i < shader->meta->resource_count; i++) out_compute->binds[i+shader->meta->buffer_count].bind = shader->meta->resources[i].bind;

	return skr_err_success;
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

	_skr_cmd_destroy_pipeline             (NULL, compute->pipeline);
	_skr_cmd_destroy_pipeline_layout      (NULL, compute->layout);
	_skr_cmd_destroy_descriptor_set_layout(NULL, compute->descriptor_layout);


	free(compute->binds);

	memset(compute, 0, sizeof(skr_compute_t));
}

void skr_compute_set_buffer(skr_compute_t* compute, const char* name, skr_buffer_t* buffer) {
	const sksc_shader_meta_t *meta = compute->shader->meta;

	int32_t  idx  = -1;
	uint64_t hash = skr_hash(name);
	for (uint32_t i = 0; i < meta->buffer_count; i++) {
		if (meta->buffers[i].name_hash == hash) {
			idx = i;
			break; 
		}
	}

	if (idx >= 0) {
		compute->binds[idx].buffer = buffer;
		return;
	}

	// StructuredBuffers look like buffers, but HLSL treats them like textures/resources
	for (uint32_t i = 0; i < meta->resource_count; i++) {
		if (meta->resources[i].name_hash == hash) {
			idx  = i;
			break; 
		}
	}

	if (idx >= 0) {
		compute->binds[meta->buffer_count + idx].buffer = buffer;
	} else {
		skr_logf(skr_log_warning, "Buffer name '%s' not found", name);
	}
	return;
}

void skr_compute_set_tex(skr_compute_t* compute, const char* name, skr_tex_t* texture) {
	const sksc_shader_meta_t *meta = compute->shader->meta;

	int32_t  idx  = -1;
	uint64_t hash = skr_hash(name);
	for (uint32_t i = 0; i < meta->resource_count; i++) {
		if (meta->resources[i].name_hash == hash) {
			idx  = i;
			break; 
		}
	}

	if (idx == -1) {
		skr_logf(skr_log_warning, "Texture name '%s' not found", name);
		return;
	}

	compute->binds[meta->buffer_count + idx].texture = texture;
}

void skr_compute_execute(skr_compute_t* compute, uint32_t x, uint32_t y, uint32_t z) {
	if (!skr_compute_is_valid(compute)) return;

	_skr_cmd_ctx_t ctx = _skr_cmd_acquire();
	VkCommandBuffer cmd = ctx.cmd;
	if (!cmd) {
		skr_log(skr_log_warning, "skr_compute_execute failed to acquire command buffer");
		return;
	}

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute->pipeline);

	// Transition all bound textures to appropriate layouts before dispatch
	const sksc_shader_meta_t* meta = compute->shader->meta;
	for (uint32_t i = 0; i < compute->bind_count; i++) {
		skr_material_bind_t *res = &compute->binds[i];
		if      (res->bind.register_type == skr_register_readwrite_tex && res->texture) {_skr_tex_transition_for_storage    (cmd, res->texture); }
		else if (res->bind.register_type == skr_register_texture       && res->texture) {_skr_tex_transition_for_shader_read(cmd, res->texture, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); }
	}

	VkWriteDescriptorSet   writes      [32];
	VkDescriptorBufferInfo buffer_infos[16];
	VkDescriptorImageInfo  image_infos [16];
	uint32_t write_ct  = 0;
	uint32_t buffer_ct = 0;
	uint32_t image_ct  = 0;
	_skr_material_add_writes(compute->binds, compute->bind_count, NULL, 0,
		writes,       sizeof(writes      )/sizeof(writes      [0]),
		buffer_infos, sizeof(buffer_infos)/sizeof(buffer_infos[0]),
		image_infos,  sizeof(image_infos )/sizeof(image_infos [0]),
		&write_ct, &buffer_ct, &image_ct);

	//_skr_log_descriptor_writes(writes, buffer_infos, image_infos, write_ct, buffer_ct, image_ct);

	_skr_bind_descriptors(cmd, ctx.descriptor_pool, VK_PIPELINE_BIND_POINT_COMPUTE,
	                      compute->layout, compute->descriptor_layout, writes, write_ct);

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

	_skr_cmd_release(cmd);
}

void skr_compute_execute_indirect(skr_compute_t* compute, skr_buffer_t* indirect_args) {
	if (!skr_compute_is_valid(compute) || !indirect_args) return;

	_skr_cmd_ctx_t ctx = _skr_cmd_acquire();
	VkCommandBuffer cmd = ctx.cmd;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute->pipeline);

	VkWriteDescriptorSet   writes      [32];
	VkDescriptorBufferInfo buffer_infos[16];
	VkDescriptorImageInfo  image_infos [16];
	uint32_t write_ct = 0;
	uint32_t buffer_ct = 0;
	uint32_t image_ct = 0;
	_skr_material_add_writes(compute->binds, compute->bind_count, NULL, 0,
		writes,       sizeof(writes      )/sizeof(writes      [0]),
		buffer_infos, sizeof(buffer_infos)/sizeof(buffer_infos[0]),
		image_infos,  sizeof(image_infos )/sizeof(image_infos [0]),
		&write_ct, &buffer_ct, &image_ct);

	_skr_bind_descriptors(cmd, ctx.descriptor_pool, VK_PIPELINE_BIND_POINT_COMPUTE,
	                      compute->layout, compute->descriptor_layout, writes, write_ct);

	vkCmdDispatchIndirect(cmd, indirect_args->buffer, 0);
	_skr_cmd_release(cmd);
}
