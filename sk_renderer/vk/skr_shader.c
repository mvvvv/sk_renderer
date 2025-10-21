// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "_sk_renderer.h"
#include "skr_shader.h"
#include "../skr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Shader stage creation
///////////////////////////////////////////////////////////////////////////////

skr_shader_stage_t _skr_shader_stage_create(const void* shader_data, size_t shader_size, skr_stage_ type) {
	skr_shader_stage_t stage = {0};
	stage.type               = type;

	VkShaderModuleCreateInfo create_info = {
		.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = shader_size,
		.pCode    = (const uint32_t*)shader_data,
	};

	if (vkCreateShaderModule(_skr_vk.device, &create_info, NULL, &stage.shader) != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to create shader module\n");
		return stage;
	}

	return stage;
}

void _skr_shader_stage_destroy(skr_shader_stage_t* stage) {
	if (!stage || stage->shader == VK_NULL_HANDLE) return;

	vkDestroyShaderModule(_skr_vk.device, stage->shader, NULL);
	memset(stage, 0, sizeof(skr_shader_stage_t));
}

skr_shader_stage_t _skr_shader_file_create_stage(const sksc_shader_file_t* file, skr_stage_ stage) {
#if defined(SKR_VK)
	skr_shader_lang_ language = skr_shader_lang_spirv;
#elif defined(skr_D3D11) || defined(skr_DIRECT3D12)
	skr_shader_lang_ language = skr_shader_lang_hlsl;
#endif

	for (uint32_t i = 0; i < file->stage_count; i++) {
		if (file->stages[i].language == language && file->stages[i].stage == stage)
			return _skr_shader_stage_create(file->stages[i].code, file->stages[i].code_size, stage);
	}
	skr_shader_stage_t empty = {0};
	return empty;
}

///////////////////////////////////////////////////////////////////////////////
// Shader creation
///////////////////////////////////////////////////////////////////////////////

skr_shader_t _skr_shader_create_manual(sksc_shader_meta_t* meta, skr_shader_stage_t v_shader,
                                       skr_shader_stage_t p_shader, skr_shader_stage_t c_shader) {
	skr_shader_t shader  = {0};
	shader.meta          = meta;
	shader.vertex_stage  = v_shader;
	shader.pixel_stage   = p_shader;
	shader.compute_stage = c_shader;

	if (meta) {
		sksc_shader_meta_reference(meta);
	}

	return shader;
}

skr_shader_t skr_shader_create(const void* shader_data, size_t data_size) {
	sksc_shader_file_t file = {0};

	sksc_result_ result = sksc_shader_file_load_memory(shader_data, data_size, &file);
	if (result != sksc_result_success) {
		const char* extra;
		switch(result) {
			case sksc_result_bad_format:    extra = "unrecognized format"; break;
			case sksc_result_old_version:   extra = "old version";         break;
			case sksc_result_out_of_memory: extra = "out of memory";       break;
			case sksc_result_corrupt_data:  extra = "corrupt data";        break;
			default:                        extra = "unknown";             break;
		}
		skr_logf(skr_log_critical, "Failed to load shader file: %s", extra);
		skr_shader_t empty = {0};
		return empty;
	}

	// Create shader stages based on what's in the file
	skr_shader_stage_t v_stage = _skr_shader_file_create_stage(&file, skr_stage_vertex);
	skr_shader_stage_t p_stage = _skr_shader_file_create_stage(&file, skr_stage_pixel);
	skr_shader_stage_t c_stage = _skr_shader_file_create_stage(&file, skr_stage_compute);

	skr_shader_t shader = _skr_shader_create_manual(file.meta, v_stage, p_stage, c_stage);

	// Don't destroy meta here, it's now owned by the shader
	// Just clean up the file structure
	for (uint32_t i = 0; i < file.stage_count; i++) {
		free(file.stages[i].code);
	}
	free(file.stages);

	return shader;
}

bool skr_shader_is_valid(const skr_shader_t* shader) {
	if (!shader) return false;
	return shader->vertex_stage.shader  != VK_NULL_HANDLE ||
	       shader->pixel_stage.shader   != VK_NULL_HANDLE ||
	       shader->compute_stage.shader != VK_NULL_HANDLE;
}

void skr_shader_destroy(skr_shader_t* shader) {
	if (!shader) return;

	_skr_shader_stage_destroy(&shader->vertex_stage);
	_skr_shader_stage_destroy(&shader->pixel_stage);
	_skr_shader_stage_destroy(&shader->compute_stage);

	if (shader->meta) {
		sksc_shader_meta_release(shader->meta);
		shader->meta = NULL;
	}

	memset(shader, 0, sizeof(skr_shader_t));
}

skr_bind_t skr_shader_get_bind(const skr_shader_t* shader, const char* bind_name) {
	if (!shader || !shader->meta) {
		skr_bind_t empty = {0};
		return empty;
	}

	return sksc_shader_meta_get_bind(shader->meta, bind_name);
}

void skr_shader_set_name(skr_shader_t* shader, const char* name) {
	if (!shader) return;

	char stage_name[256];

	if (shader->vertex_stage.shader != VK_NULL_HANDLE) {
		snprintf(stage_name, sizeof(stage_name), "%s_vert", name);
		_skr_set_debug_name(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)shader->vertex_stage.shader, stage_name);
	}
	if (shader->pixel_stage.shader != VK_NULL_HANDLE) {
		snprintf(stage_name, sizeof(stage_name), "%s_frag", name);
		_skr_set_debug_name(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)shader->pixel_stage.shader, stage_name);
	}
	if (shader->compute_stage.shader != VK_NULL_HANDLE) {
		snprintf(stage_name, sizeof(stage_name), "%s_comp", name);
		_skr_set_debug_name(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)shader->compute_stage.shader, stage_name);
	}
}
