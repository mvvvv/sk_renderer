// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "sk_renderer.h"
#include "_sk_renderer.h"

#include "skr_vulkan.h"
#include "skr_pipeline.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

///////////////////////////////////////////////////////////////////////////////

skr_err_ skr_material_create(skr_material_info_t info, skr_material_t* out_material) {
	if (!out_material) return skr_err_invalid_parameter;

	// Zero out immediately
	*out_material = (skr_material_t){};

	if (!info.shader || !skr_shader_is_valid(info.shader)) {
		skr_log(skr_log_warning, "Cannot create material with invalid shader");
		return skr_err_invalid_parameter;
	}

	// Store material info
	out_material->info = info;

	// Default write_mask to skr_write_default if not specified
	if (out_material->info.write_mask == 0) {
		out_material->info.write_mask = skr_write_default;
	}

	if (out_material->info.shader->meta) {
		sksc_shader_meta_reference(out_material->info.shader->meta);
	}

	// Allocate material parameter buffer if shader has $Global buffer
	const sksc_shader_meta_t* meta = out_material->info.shader->meta;
	if (meta && meta->global_buffer_id >= 0) {
		sksc_shader_buffer_t* global_buffer = &meta->buffers[meta->global_buffer_id];
		out_material->param_buffer_size = global_buffer->size;
		out_material->param_buffer = _skr_malloc(out_material->param_buffer_size);

		if (!out_material->param_buffer) {
			skr_log(skr_log_critical, "Failed to allocate material parameter buffer");
			if (out_material->info.shader->meta) {
				sksc_shader_meta_release(out_material->info.shader->meta);
			}
			*out_material = (skr_material_t){};
			return skr_err_out_of_memory;
		}

		// Initialize with default values if available
		if (global_buffer->defaults) {
			memcpy(out_material->param_buffer, global_buffer->defaults, out_material->param_buffer_size);
		} else {
			memset(out_material->param_buffer, 0, out_material->param_buffer_size);
		}
	}

	// Allocate memory for our material resource binds
	out_material->bind_count = meta->resource_count + meta->buffer_count;
	out_material->binds      = (skr_material_bind_t*)_skr_calloc(out_material->bind_count, sizeof(skr_material_bind_t));
	for (uint32_t i = 0; i < meta->buffer_count;   i++) out_material->binds[i                   ].bind = meta->buffers  [i].bind;
	for (uint32_t i = 0; i < meta->resource_count; i++) out_material->binds[i+meta->buffer_count].bind = meta->resources[i].bind;

	// Check if we have a SystemBuffer
	skr_bind_t system_bind = sksc_shader_meta_get_bind(meta, "SystemBuffer");
	out_material->has_system_buffer = system_bind.slot == _skr_vk.bind_settings.system_slot && system_bind.stage_bits != 0;

	// Register material with pipeline system
	out_material->pipeline_material_idx = _skr_pipeline_register_material(&out_material->info);

	if (out_material->pipeline_material_idx < 0) {
		skr_log(skr_log_critical, "Failed to register material with pipeline system");
		_skr_free(out_material->binds);
		_skr_free(out_material->param_buffer);
		if (out_material->info.shader->meta) {
			sksc_shader_meta_release(out_material->info.shader->meta);
		}
		*out_material = (skr_material_t){};
		return skr_err_device_error;
	}

	// Fill out default textures
	for (uint32_t i = 0; i < meta->resource_count; i++) {
		skr_tex_t* tex = &_skr_vk.default_tex_white;
		if      (strcmp(meta->resources[i].value, "black") == 0) tex = &_skr_vk.default_tex_black;
		else if (strcmp(meta->resources[i].value, "gray" ) == 0) tex = &_skr_vk.default_tex_gray;
		else if (strcmp(meta->resources[i].value, "grey" ) == 0) tex = &_skr_vk.default_tex_gray;
		skr_material_set_tex(out_material, meta->resources[i].name, tex);
	}

	return skr_err_success;
}

bool skr_material_is_valid(const skr_material_t* material) {
	return material && material->pipeline_material_idx >= 0;
}

void skr_material_destroy(skr_material_t* ref_material) {
	if (!ref_material) return;

	// Unregister from pipeline system
	if (ref_material->pipeline_material_idx >= 0) {
		_skr_pipeline_unregister_material(ref_material->pipeline_material_idx);
	}

	// Free allocated memory
	_skr_free(ref_material->param_buffer);
	_skr_free(ref_material->binds);

	if (ref_material->info.shader && ref_material->info.shader->meta) {
		sksc_shader_meta_release(ref_material->info.shader->meta);
	}

	*ref_material = (skr_material_t){};
	ref_material->pipeline_material_idx = -1;
}

void skr_material_set_tex(skr_material_t* ref_material, const char* name, skr_tex_t* texture) {
	const sksc_shader_meta_t *meta = ref_material->info.shader->meta;

	int32_t  idx  = -1;
	uint64_t hash = skr_hash(name);
	for (uint32_t i = 0; i < meta->resource_count; i++) {
		if (meta->resources[i].name_hash == hash) {
			idx  = i;
			break; 
		}
	}

	if (idx == -1) {
		skr_log(skr_log_warning, "Texture name '%s' not found", name);
		return;
	}

	ref_material->binds[meta->buffer_count + idx].texture = texture;
}

void skr_material_set_buffer(skr_material_t* ref_material, const char* name, skr_buffer_t* buffer) {
	const sksc_shader_meta_t *meta = ref_material->info.shader->meta;

	int32_t  idx  = -1;
	uint64_t hash = skr_hash(name);
	for (uint32_t i = 0; i < meta->buffer_count; i++) {
		if (meta->buffers[i].name_hash == hash) {
			idx = i;
			break; 
		}
	}

	if (idx >= 0) {
		ref_material->binds[idx].buffer = buffer;
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
		ref_material->binds[meta->buffer_count + idx].buffer = buffer;
	} else {
		skr_log(skr_log_warning, "Buffer name '%s' not found", name);
	}
	return;
}

void skr_material_set_params(skr_material_t* ref_material, const void* data, uint32_t size) {
	if (size != ref_material->param_buffer_size) {
		skr_log(skr_log_warning, "material_set_params: incorrect size!");
		return;
	}
	memcpy(ref_material->param_buffer, data, size);
}


///////////////////////////////////////////////////////////////////////////////
// Material parameter setters/getters
///////////////////////////////////////////////////////////////////////////////

static uint32_t _skr_shader_var_size(sksc_shader_var_ type) {
	switch (type) {
		case sksc_shader_var_int:    return sizeof(int32_t);
		case sksc_shader_var_uint:   return sizeof(uint32_t);
		case sksc_shader_var_uint8:  return sizeof(uint8_t);
		case sksc_shader_var_float:  return sizeof(float);
		case sksc_shader_var_double: return sizeof(double);
		default:                     return 0;
	}
}

void skr_material_set_param(skr_material_t* material, const char* name, sksc_shader_var_ type, uint32_t count, const void* data) {
	if (!material || !material->info.shader || !material->info.shader->meta || !material->param_buffer) return;

	int32_t var_index = sksc_shader_meta_get_var_index(material->info.shader->meta, name);
	if (var_index < 0) {
		skr_log(skr_log_warning, "Material parameter '%s' not found", name);
		return;
	}

	const sksc_shader_var_t* var = sksc_shader_meta_get_var_info(material->info.shader->meta, var_index);
	if (!var) return;

	// When type is uint8, treat count as raw byte count and skip type check
	uint32_t copy_size;
	if (type == sksc_shader_var_uint8) {
		copy_size = count;
	} else {
		if (var->type != type) {
			skr_log(skr_log_warning, "Material parameter '%s' type mismatch", name);
			return;
		}
		copy_size = _skr_shader_var_size(type) * count;
	}

	if (var->offset + copy_size > material->param_buffer_size) {
		skr_log(skr_log_warning, "Material parameter '%s' write would exceed buffer size", name);
		return;
	}

	memcpy((uint8_t*)material->param_buffer + var->offset, data, copy_size);
}

void skr_material_get_param(const skr_material_t* material, const char* name, sksc_shader_var_ type, uint32_t count, void* out_data) {
	if (!material || !material->info.shader || !material->info.shader->meta || !material->param_buffer) return;

	int32_t var_index = sksc_shader_meta_get_var_index(material->info.shader->meta, name);
	if (var_index < 0) {
		skr_log(skr_log_warning, "Material parameter '%s' not found", name);
		return;
	}

	const sksc_shader_var_t* var = sksc_shader_meta_get_var_info(material->info.shader->meta, var_index);
	if (!var) return;

	// When type is uint8, treat count as raw byte count and skip type check
	uint32_t copy_size;
	if (type == sksc_shader_var_uint8) {
		copy_size = count;
	} else {
		if (var->type != type) {
			skr_log(skr_log_warning, "Material parameter '%s' type mismatch", name);
			return;
		}
		copy_size = _skr_shader_var_size(type) * count;
	}

	if (var->offset + copy_size > material->param_buffer_size) {
		skr_log(skr_log_warning, "Material parameter '%s' read would exceed buffer size", name);
		return;
	}

	memcpy(out_data, (uint8_t*)material->param_buffer + var->offset, copy_size);
}

void _skr_material_add_writes(const skr_material_bind_t* binds, uint32_t bind_ct, const int32_t* ignore_slots, int32_t ignore_ct, VkWriteDescriptorSet* ref_writes, uint32_t write_max, VkDescriptorBufferInfo* ref_buffer_infos, uint32_t buffer_max, VkDescriptorImageInfo* ref_image_infos, uint32_t image_max, uint32_t* ref_write_ct, uint32_t* ref_buffer_ct, uint32_t* ref_image_ct) {
 	for (uint32_t i = 0; i < bind_ct; i++) {
		int32_t       slot          = binds[i].bind.slot;
		skr_register_ register_type = binds[i].bind.register_type;

		bool skip = false;
		for (int32_t s = 0; s < ignore_ct; s++) {
			if (slot == ignore_slots[s]) {
				skip = true;
				break;
			}
		}
		if (skip) continue;
		
		switch(register_type) {
		case skr_register_constant: { // cbuffer, (b in HLSL)
			if (*ref_write_ct >= write_max || *ref_buffer_ct >= buffer_max) continue;

			skr_buffer_t* buffer = _skr_vk.global_buffers[slot-SKR_BIND_SHIFT_BUFFER];
			if (!buffer)  buffer = binds[i].buffer;
			assert(buffer);

			ref_buffer_infos[*ref_buffer_ct] = (VkDescriptorBufferInfo){
				.buffer = buffer->buffer,
				.range  = buffer->size,
			};
			ref_writes[*ref_write_ct] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding      = slot,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo     = &ref_buffer_infos[*ref_buffer_ct],
			};
			(*ref_write_ct)++;
			(*ref_buffer_ct)++;
		} break;
		case skr_register_read_buffer: { // StructuredBuffer, (t in HLSL)
			if (*ref_write_ct >= write_max || *ref_buffer_ct >= buffer_max) continue;

			skr_buffer_t* buffer = _skr_vk.global_buffers[slot-SKR_BIND_SHIFT_TEXTURE];
			if (!buffer)  buffer = binds[i].buffer;
			assert(buffer);

			ref_buffer_infos[*ref_buffer_ct] = (VkDescriptorBufferInfo){
				.buffer = buffer->buffer,
				.range  = buffer->size,
			};
			ref_writes[*ref_write_ct] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding      = slot,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo     = &ref_buffer_infos[*ref_buffer_ct],
			};
			(*ref_write_ct)++;
			(*ref_buffer_ct)++;
		} break;
		case skr_register_texture: { // Textures (Texture2D, etc.) (t in HLSL)
			if (*ref_write_ct >= write_max || *ref_image_ct >= image_max) continue;

			skr_tex_t* tex = _skr_vk.global_textures[slot-SKR_BIND_SHIFT_TEXTURE];
			if (!tex)  tex = binds[i].texture;
			assert(tex);

			ref_image_infos[*ref_image_ct] = (VkDescriptorImageInfo){
				.sampler     = tex->sampler,
				.imageView   = tex->view,
				.imageLayout = (tex->flags & skr_tex_flags_compute)
					? VK_IMAGE_LAYOUT_GENERAL
					: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			ref_writes[*ref_write_ct] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding      = slot,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo      = &ref_image_infos[*ref_image_ct],
			};
			(*ref_write_ct)++;
			(*ref_image_ct)++;
		} break;
		case skr_register_readwrite: { // RWStructuredBuffer (u in HLSL)
			if (*ref_write_ct >= write_max || *ref_buffer_ct >= buffer_max) continue;

			skr_buffer_t* buffer = _skr_vk.global_buffers[slot-SKR_BIND_SHIFT_UAV];
			if (!buffer)  buffer = binds[i].buffer;
			assert(buffer);

			ref_buffer_infos[*ref_buffer_ct] = (VkDescriptorBufferInfo){
				.buffer = buffer->buffer,
				.range  = buffer->size,
			};
			ref_writes[*ref_write_ct] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding      = slot,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo     = &ref_buffer_infos[*ref_buffer_ct],
			};
			(*ref_write_ct)++;
			(*ref_buffer_ct)++;
		} break;
		case skr_register_readwrite_tex: { // Storage images (RWTexture2D, etc.)
			if (*ref_write_ct >= write_max || *ref_image_ct >= image_max) continue;

			skr_tex_t* tex = _skr_vk.global_textures[slot-SKR_BIND_SHIFT_UAV];
			if (!tex)  tex = binds[i].texture;
			assert(tex);

			ref_image_infos[*ref_image_ct] = (VkDescriptorImageInfo){
				.sampler     = tex->sampler,
				.imageView   = tex->view,
				.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			};
			ref_writes[*ref_write_ct] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding      = slot,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.pImageInfo      = &ref_image_infos[*ref_image_ct],
			};
			(*ref_write_ct)++;
			(*ref_image_ct)++;
		} break; 
		default: break;}
	}
}