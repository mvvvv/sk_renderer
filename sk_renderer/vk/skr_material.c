// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "sk_renderer.h"
#include "_sk_renderer.h"

#include "skr_vulkan.h"
#include "skr_pipeline.h"
#include "../skr_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

///////////////////////////////////////////////////////////////////////////////

skr_material_t skr_material_create(skr_material_info_t info) {
	skr_material_t material = {0};

	if (!info.shader || !skr_shader_is_valid(info.shader)) {
		skr_log(skr_log_warning, "Cannot create material with invalid shader");
		return material;
	}

	// Store material info
	material.info = info;
	if (material.info.shader->meta) {
		sksc_shader_meta_reference(material.info.shader->meta);
	}

	// Register material with pipeline system
	material.pipeline_material_idx = _skr_pipeline_register_material(&material.info);

	if (material.pipeline_material_idx < 0) {
		skr_log(skr_log_critical, "Failed to register material with pipeline system");
		skr_material_destroy(&material);
		return material;
	}

	return material;
}

bool skr_material_is_valid(const skr_material_t* material) {
	return material && material->pipeline_material_idx >= 0;
}

void skr_material_destroy(skr_material_t* material) {
	if (!material) return;

	// Unregister from pipeline system
	if (material->pipeline_material_idx >= 0) {
		_skr_pipeline_unregister_material(material->pipeline_material_idx);
	}

	if (material->info.shader && material->info.shader->meta) {
		sksc_shader_meta_release(material->info.shader->meta);
	}

	memset(material, 0, sizeof(skr_material_t));
	material->pipeline_material_idx = -1;
}

void _skr_material_rebuild_descriptors(skr_material_t* material) {
	if (!material || !material->info.shader || !material->info.shader->meta) {
		material->descriptor_write_count = 0;
		material->descriptors_dirty = false;
		return;
	}

	uint32_t write_count = 0;
	uint32_t buffer_info_count = 0;
	uint32_t image_info_count = 0;
	const sksc_shader_meta_t* meta = material->info.shader->meta;

	// Build buffer descriptors (material or global placeholder)
	for (uint32_t i = 0; i < meta->buffer_count; i++) {
		int32_t slot = meta->buffers[i].bind.slot;
		if (slot >= 0 && slot < 16) {
			skr_buffer_t* buffer = material->buffers[slot];
			if (buffer || _skr_vk.global_buffers[slot]) {
				// Use material buffer if set, otherwise globals will be patched in later
				if (buffer) {
					material->buffer_infos[slot] = (VkDescriptorBufferInfo){
						.buffer = buffer->buffer,
						.offset = 0,
						.range  = buffer->size,
					};
				} else if (_skr_vk.global_buffers[slot]) {
					material->buffer_infos[slot] = (VkDescriptorBufferInfo){
						.buffer = _skr_vk.global_buffers[slot]->buffer,
						.offset = 0,
						.range  = _skr_vk.global_buffers[slot]->size,
					};
				}

				material->descriptor_writes[write_count++] = (VkWriteDescriptorSet){
					.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet          = VK_NULL_HANDLE,
					.dstBinding      = slot,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.pBufferInfo     = &material->buffer_infos[slot],
				};
			}
		}
	}

	// Build resource descriptors (textures and storage buffers)
	for (uint32_t i = 0; i < meta->resource_count; i++) {
		int32_t slot = meta->resources[i].bind.slot;
		uint8_t register_type = meta->resources[i].bind.register_type;

		// Handle storage buffers (StructuredBuffer, RWStructuredBuffer)
		if ((register_type == skr_register_read_buffer || register_type == skr_register_readwrite) &&
		    slot >= 0 && slot < 16) {
			skr_buffer_t* buffer = material->buffers[slot];
			if (buffer || _skr_vk.global_buffers[slot]) {
				// Use material buffer if set, otherwise globals will be patched in later
				if (buffer) {
					material->buffer_infos[slot] = (VkDescriptorBufferInfo){
						.buffer = buffer->buffer,
						.offset = 0,
						.range  = buffer->size,
					};
				} else if (_skr_vk.global_buffers[slot]) {
					material->buffer_infos[slot] = (VkDescriptorBufferInfo){
						.buffer = _skr_vk.global_buffers[slot]->buffer,
						.offset = 0,
						.range  = _skr_vk.global_buffers[slot]->size,
					};
				}

				material->descriptor_writes[write_count++] = (VkWriteDescriptorSet){
					.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet          = VK_NULL_HANDLE,
					.dstBinding      = slot,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.pBufferInfo     = &material->buffer_infos[slot],
				};
			}
		}
		// Handle textures (Texture2D, etc.)
		else if (register_type == skr_register_texture && slot >= 0 && slot < 16 &&
		         (material->textures[slot] || _skr_vk.global_textures[slot])) {
			skr_tex_t* tex = material->textures[slot] ? material->textures[slot] : _skr_vk.global_textures[slot];
			VkImageLayout layout = (tex->flags & skr_tex_flags_compute)
				? VK_IMAGE_LAYOUT_GENERAL
				: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			material->image_infos[image_info_count] = (VkDescriptorImageInfo){
				.sampler     = tex->sampler,
				.imageView   = tex->view,
				.imageLayout = layout,
			};

			material->descriptor_writes[write_count++] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet          = VK_NULL_HANDLE,
				.dstBinding      = slot,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo      = &material->image_infos[image_info_count++],
			};
		}
		// Handle storage images (RWTexture2D, etc.)
		else if (register_type == skr_register_readwrite_tex && slot >= 0 && slot < 16 &&
		         (material->textures[slot] || _skr_vk.global_textures[slot])) {
			skr_tex_t* tex = material->textures[slot] ? material->textures[slot] : _skr_vk.global_textures[slot];
			material->image_infos[image_info_count] = (VkDescriptorImageInfo){
				.sampler     = VK_NULL_HANDLE,
				.imageView   = tex->view,
				.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			};

			material->descriptor_writes[write_count++] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet          = VK_NULL_HANDLE,
				.dstBinding      = slot,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.pImageInfo      = &material->image_infos[image_info_count++],
			};
		}
	}

	material->descriptor_write_count = write_count;
	material->descriptors_dirty = false;
}

void skr_material_set_tex(skr_material_t* material, int32_t bind, skr_tex_t* texture) {
	if (!material || bind < 0 || bind >= 16) return;
	material->textures[bind] = texture;
	material->descriptors_dirty = true;

	// Queue transition for this texture (will be flushed before next render pass)
	// Only enqueue if the texture actually needs a transition
	if (texture) {
		const sksc_shader_meta_t* meta = material->info.shader->meta;
		for (uint32_t i = 0; i < meta->resource_count; i++) {
			if (meta->resources[i].bind.slot == bind) {
				uint8_t type = (meta->resources[i].bind.register_type == skr_register_readwrite_tex) ? 1 : 0;

				// Only enqueue if transition is needed
				if (_skr_tex_needs_transition(texture, type)) {
					_skr_tex_transition_enqueue(texture, type);
				}
				break;
			}
		}
	}
}

void skr_material_set_buffer(skr_material_t* material, int32_t bind, skr_buffer_t* buffer) {
	if (!material || bind < 0 || bind >= 16) return;
	material->buffers[bind] = buffer;
	material->descriptors_dirty = true;
}

// Update buffer infos with current global data (call before draw)
void _skr_material_update_globals(skr_material_t* material) {
	if (!material->info.shader || !material->info.shader->meta) return;

	const sksc_shader_meta_t* meta = material->info.shader->meta;

	// Update constant buffer globals
	for (uint32_t i = 0; i < meta->buffer_count; i++) {
		int32_t slot = meta->buffers[i].bind.slot;
		if (slot >= 0 && slot < 16 && !material->buffers[slot] && _skr_vk.global_buffers[slot]) {
			// Update the buffer_info that the descriptor write already points to
			material->buffer_infos[slot].buffer = _skr_vk.global_buffers[slot]->buffer;
			material->buffer_infos[slot].offset = 0;
			material->buffer_infos[slot].range  = _skr_vk.global_buffers[slot]->size;
		}
	}

	// Update resource globals (textures and storage buffers)
	for (uint32_t i = 0; i < meta->resource_count; i++) {
		int32_t slot = meta->resources[i].bind.slot;
		uint8_t register_type = meta->resources[i].bind.register_type;

		// Handle storage buffers (StructuredBuffer, RWStructuredBuffer)
		if ((register_type == skr_register_read_buffer || register_type == skr_register_readwrite) &&
		    slot >= 0 && slot < 16 && !material->buffers[slot] && _skr_vk.global_buffers[slot]) {
			material->buffer_infos[slot].buffer = _skr_vk.global_buffers[slot]->buffer;
			material->buffer_infos[slot].offset = 0;
			material->buffer_infos[slot].range  = _skr_vk.global_buffers[slot]->size;
		}

		// Handle textures (Texture2D, RWTexture, etc.)
		if ((register_type == skr_register_texture || register_type == skr_register_readwrite_tex) &&
		    slot >= 0 && slot < 16 && !material->textures[slot] && _skr_vk.global_textures[slot]) {
			// Update the image_info that the descriptor write already points to
			material->image_infos[slot].imageView   = _skr_vk.global_textures[slot]->view;
			material->image_infos[slot].sampler     = _skr_vk.global_textures[slot]->sampler;
			material->image_infos[slot].imageLayout = (register_type == skr_register_readwrite_tex) ?
				VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
	}
}
