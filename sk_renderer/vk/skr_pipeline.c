// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "_sk_renderer.h"

#include "skr_pipeline.h"
#include "skr_conversions.h"
#include "../skr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////

#define SKR_MAX_MATERIALS    64
#define SKR_MAX_RENDERPASSES 16
#define SKR_MAX_VERTFORMATS  8

///////////////////////////////////////////////////////////////////////////////
// Types
///////////////////////////////////////////////////////////////////////////////

typedef struct {
	skr_material_info_t   info;
	VkPipelineLayout      layout;
	VkDescriptorSetLayout descriptor_layout;
	bool                  active;
} _skr_pipeline_material_slot_t;

typedef struct {
	skr_pipeline_renderpass_key_t key;
	VkRenderPass                  render_pass;
	bool                          active;
} _skr_pipeline_renderpass_slot_t;

typedef struct {
	skr_vert_type_t vert_type;
	bool            active;
} _skr_pipeline_vertformat_slot_t;

typedef struct {
	VkPipeline pipeline;
	bool       created;
} _skr_pipeline_entry_t;

typedef struct {
	_skr_pipeline_material_slot_t    materials   [SKR_MAX_MATERIALS];
	_skr_pipeline_renderpass_slot_t  renderpasses[SKR_MAX_RENDERPASSES];
	_skr_pipeline_vertformat_slot_t  vertformats [SKR_MAX_VERTFORMATS];
	_skr_pipeline_entry_t            pipelines   [SKR_MAX_MATERIALS][SKR_MAX_RENDERPASSES][SKR_MAX_VERTFORMATS];
	int32_t                          material_count;
	int32_t                          renderpass_count;
	int32_t                          vertformat_count;
} _skr_pipeline_cache_t;

///////////////////////////////////////////////////////////////////////////////
// State
///////////////////////////////////////////////////////////////////////////////

static _skr_pipeline_cache_t _skr_pipeline_cache = {0};

///////////////////////////////////////////////////////////////////////////////
// Forward declarations
///////////////////////////////////////////////////////////////////////////////

static VkRenderPass          _skr_pipeline_create_renderpass       (const skr_pipeline_renderpass_key_t* key);
static VkDescriptorSetLayout _skr_pipeline_create_descriptor_layout(const sksc_shader_meta_t* meta);
static VkPipelineLayout      _skr_pipeline_create_layout           (VkDescriptorSetLayout descriptor_layout);
static VkPipeline            _skr_pipeline_create                  (int32_t material_idx, int32_t renderpass_idx, int32_t vertformat_idx);

///////////////////////////////////////////////////////////////////////////////

void _skr_pipeline_init() {
	memset(&_skr_pipeline_cache, 0, sizeof(_skr_pipeline_cache));
}

void _skr_pipeline_shutdown() {
	// Destroy all pipelines
	for (int32_t m = 0; m < SKR_MAX_MATERIALS; m++) {
		for (int32_t r = 0; r < SKR_MAX_RENDERPASSES; r++) {
			for (int32_t v = 0; v < SKR_MAX_VERTFORMATS; v++) {
				if (_skr_pipeline_cache.pipelines[m][r][v].created &&
				    _skr_pipeline_cache.pipelines[m][r][v].pipeline != VK_NULL_HANDLE) {
					vkDestroyPipeline(_skr_vk.device, _skr_pipeline_cache.pipelines[m][r][v].pipeline, NULL);
				}
			}
		}
	}

	// Destroy material resources
	for (int32_t m = 0; m < SKR_MAX_MATERIALS; m++) {
		if (_skr_pipeline_cache.materials[m].active) {
			if (_skr_pipeline_cache.materials[m].layout != VK_NULL_HANDLE) {
				vkDestroyPipelineLayout(_skr_vk.device, _skr_pipeline_cache.materials[m].layout, NULL);
			}
			if (_skr_pipeline_cache.materials[m].descriptor_layout != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(_skr_vk.device, _skr_pipeline_cache.materials[m].descriptor_layout, NULL);
			}
		}
	}

	// Destroy render passes
	for (int32_t r = 0; r < SKR_MAX_RENDERPASSES; r++) {
		if (_skr_pipeline_cache.renderpasses[r].active &&
		    _skr_pipeline_cache.renderpasses[r].render_pass != VK_NULL_HANDLE) {
			vkDestroyRenderPass(_skr_vk.device, _skr_pipeline_cache.renderpasses[r].render_pass, NULL);
		}
	}

	memset(&_skr_pipeline_cache, 0, sizeof(_skr_pipeline_cache));
}

int32_t _skr_pipeline_register_material(const skr_material_info_t* info) {
	// Find existing or free slot
	int32_t free_slot = -1;
	for (int32_t i = 0; i < SKR_MAX_MATERIALS; i++) {
		if (_skr_pipeline_cache.materials[i].active) {
			// Check if this material already exists
			if (memcmp(&_skr_pipeline_cache.materials[i].info, info, sizeof(skr_material_info_t)) == 0) {
				return i;
			}
		} else if (free_slot == -1) {
			free_slot = i;
		}
	}

	if (free_slot == -1) {
		skr_log(skr_log_critical, "Pipeline material cache full!");
		return -1;
	}

	// Register new material
	_skr_pipeline_cache.materials[free_slot].info              = *info;
	_skr_pipeline_cache.materials[free_slot].descriptor_layout = _skr_pipeline_create_descriptor_layout(info->shader->meta);
	_skr_pipeline_cache.materials[free_slot].layout            = _skr_pipeline_create_layout           (_skr_pipeline_cache.materials[free_slot].descriptor_layout);
	_skr_pipeline_cache.materials[free_slot].active            = true;

	if (free_slot >= _skr_pipeline_cache.material_count) {
		_skr_pipeline_cache.material_count = free_slot + 1;
	}

	// Generate and set debug name for pipeline layout
	char name[256];
	const char* shader_name = (info->shader->meta && info->shader->meta->name[0]) ? info->shader->meta->name : "unknown";
	snprintf(name, sizeof(name), "layout_%s_", shader_name);
	_skr_append_material_config(name, sizeof(name), info);
	_skr_set_debug_name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)_skr_pipeline_cache.materials[free_slot].layout, name);

	// Generate debug name based on shader
	snprintf(name, sizeof(name), "layoutdesc_%s_", info->shader->meta->name[0] ? info->shader->meta->name : "unknown");
	_skr_append_material_config(name, sizeof(name), info);
	_skr_set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, (uint64_t)_skr_pipeline_cache.materials[free_slot].descriptor_layout, name);

	return free_slot;
}

int32_t _skr_pipeline_register_renderpass(const skr_pipeline_renderpass_key_t* key) {
	// Find existing or free slot
	int32_t free_slot = -1;
	for (int32_t i = 0; i < SKR_MAX_RENDERPASSES; i++) {
		if (_skr_pipeline_cache.renderpasses[i].active) {
			// Check if this render pass already exists
			if (memcmp(&_skr_pipeline_cache.renderpasses[i].key, key, sizeof(skr_pipeline_renderpass_key_t)) == 0) {
				return i;
			}
		} else if (free_slot == -1) {
			free_slot = i;
		}
	}

	if (free_slot == -1) {
		skr_log(skr_log_critical, "Pipeline renderpass cache full!");
		return -1;
	}

	// Register new render pass - create and own it
	_skr_pipeline_cache.renderpasses[free_slot].key         = *key;
	_skr_pipeline_cache.renderpasses[free_slot].render_pass = _skr_pipeline_create_renderpass(key);
	_skr_pipeline_cache.renderpasses[free_slot].active      = true;

	if (free_slot >= _skr_pipeline_cache.renderpass_count) {
		_skr_pipeline_cache.renderpass_count = free_slot + 1;
	}

	return free_slot;
}

void _skr_pipeline_unregister_material(int32_t material_idx) {
	if (material_idx < 0 || material_idx >= SKR_MAX_MATERIALS) return;
	if (!_skr_pipeline_cache.materials[material_idx].active) return;

	// Destroy all pipelines using this material
	for (int32_t r = 0; r < SKR_MAX_RENDERPASSES; r++) {
		for (int32_t v = 0; v < SKR_MAX_VERTFORMATS; v++) {
			if (_skr_pipeline_cache.pipelines[material_idx][r][v].created &&
			    _skr_pipeline_cache.pipelines[material_idx][r][v].pipeline != VK_NULL_HANDLE) {
				vkDestroyPipeline(_skr_vk.device, _skr_pipeline_cache.pipelines[material_idx][r][v].pipeline, NULL);
				_skr_pipeline_cache.pipelines[material_idx][r][v].pipeline = VK_NULL_HANDLE;
				_skr_pipeline_cache.pipelines[material_idx][r][v].created  = false;
			}
		}
	}

	// Destroy material resources
	if (_skr_pipeline_cache.materials[material_idx].layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(_skr_vk.device, _skr_pipeline_cache.materials[material_idx].layout, NULL);
	}
	if (_skr_pipeline_cache.materials[material_idx].descriptor_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(_skr_vk.device, _skr_pipeline_cache.materials[material_idx].descriptor_layout, NULL);
	}

	_skr_pipeline_cache.materials[material_idx].active = false;
}

void _skr_pipeline_unregister_renderpass(int32_t renderpass_idx) {
	if (renderpass_idx < 0 || renderpass_idx >= SKR_MAX_RENDERPASSES) return;
	if (!_skr_pipeline_cache.renderpasses[renderpass_idx].active) return;

	// Destroy all pipelines using this render pass
	for (int32_t m = 0; m < SKR_MAX_MATERIALS; m++) {
		for (int32_t v = 0; v < SKR_MAX_VERTFORMATS; v++) {
			if (_skr_pipeline_cache.pipelines[m][renderpass_idx][v].created &&
			    _skr_pipeline_cache.pipelines[m][renderpass_idx][v].pipeline != VK_NULL_HANDLE) {
				vkDestroyPipeline(_skr_vk.device, _skr_pipeline_cache.pipelines[m][renderpass_idx][v].pipeline, NULL);
				_skr_pipeline_cache.pipelines[m][renderpass_idx][v].pipeline = VK_NULL_HANDLE;
				_skr_pipeline_cache.pipelines[m][renderpass_idx][v].created  = false;
			}
		}
	}

	// Destroy render pass (we own it now)
	if (_skr_pipeline_cache.renderpasses[renderpass_idx].render_pass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(_skr_vk.device, _skr_pipeline_cache.renderpasses[renderpass_idx].render_pass, NULL);
	}

	_skr_pipeline_cache.renderpasses[renderpass_idx].active = false;
}

int32_t _skr_pipeline_register_vertformat(skr_vert_type_t vert_type) {
	// Find existing or free slot
	int32_t free_slot = -1;
	for (int32_t i = 0; i < SKR_MAX_VERTFORMATS; i++) {
		if (_skr_pipeline_cache.vertformats[i].active) {
			// Check if this vertex format already exists (pointer comparison)
			if (memcmp(&_skr_pipeline_cache.vertformats[i].vert_type, &vert_type, sizeof(skr_vert_type_t)) == 0) {
				return i;
			}
		} else if (free_slot == -1) {
			free_slot = i;
		}
	}

	if (free_slot == -1) {
		skr_log(skr_log_critical, "Pipeline vertex format cache full!");
		return -1;
	}

	// Register new vertex format (just store pointer, mesh owns it)
	_skr_pipeline_cache.vertformats[free_slot].vert_type = vert_type;
	_skr_pipeline_cache.vertformats[free_slot].active    = true;

	if (free_slot >= _skr_pipeline_cache.vertformat_count) {
		_skr_pipeline_cache.vertformat_count = free_slot + 1;
	}

	return free_slot;
}

void _skr_pipeline_unregister_vertformat(int32_t vertformat_idx) {
	if (vertformat_idx < 0 || vertformat_idx >= SKR_MAX_VERTFORMATS) return;
	if (!_skr_pipeline_cache.vertformats[vertformat_idx].active) return;

	// Destroy all pipelines using this vertex format
	for (int32_t m = 0; m < SKR_MAX_MATERIALS; m++) {
		for (int32_t r = 0; r < SKR_MAX_RENDERPASSES; r++) {
			if (_skr_pipeline_cache.pipelines[m][r][vertformat_idx].created &&
			    _skr_pipeline_cache.pipelines[m][r][vertformat_idx].pipeline != VK_NULL_HANDLE) {
				vkDestroyPipeline(_skr_vk.device, _skr_pipeline_cache.pipelines[m][r][vertformat_idx].pipeline, NULL);
				_skr_pipeline_cache.pipelines[m][r][vertformat_idx].pipeline = VK_NULL_HANDLE;
				_skr_pipeline_cache.pipelines[m][r][vertformat_idx].created  = false;
			}
		}
	}

	_skr_pipeline_cache.vertformats[vertformat_idx].active = false;
}

VkPipeline _skr_pipeline_get(int32_t material_idx, int32_t renderpass_idx, int32_t vertformat_idx) {
	if (material_idx   < 0 || material_idx   >= SKR_MAX_MATERIALS)    return VK_NULL_HANDLE;
	if (renderpass_idx < 0 || renderpass_idx >= SKR_MAX_RENDERPASSES) return VK_NULL_HANDLE;
	if (vertformat_idx < 0 || vertformat_idx >= SKR_MAX_VERTFORMATS)  return VK_NULL_HANDLE;
	if (!_skr_pipeline_cache.materials   [material_idx  ].active)     return VK_NULL_HANDLE;
	if (!_skr_pipeline_cache.renderpasses[renderpass_idx].active)     return VK_NULL_HANDLE;
	if (!_skr_pipeline_cache.vertformats [vertformat_idx].active)     return VK_NULL_HANDLE;

	// Check if pipeline already exists
	if (_skr_pipeline_cache.pipelines[material_idx][renderpass_idx][vertformat_idx].created) {
		return _skr_pipeline_cache.pipelines[material_idx][renderpass_idx][vertformat_idx].pipeline;
	}

	// Create pipeline
	VkPipeline pipeline = _skr_pipeline_create(material_idx, renderpass_idx, vertformat_idx);
	_skr_pipeline_cache.pipelines[material_idx][renderpass_idx][vertformat_idx].pipeline = pipeline;
	_skr_pipeline_cache.pipelines[material_idx][renderpass_idx][vertformat_idx].created  = true;

	return pipeline;
}

VkPipelineLayout _skr_pipeline_get_layout(int32_t material_idx) {
	if (material_idx < 0 || material_idx >= SKR_MAX_MATERIALS) return VK_NULL_HANDLE;
	if (!_skr_pipeline_cache.materials[material_idx].active)   return VK_NULL_HANDLE;

	return _skr_pipeline_cache.materials[material_idx].layout;
}

VkDescriptorSetLayout _skr_pipeline_get_descriptor_layout(int32_t material_idx) {
	if (material_idx < 0 || material_idx >= SKR_MAX_MATERIALS) return VK_NULL_HANDLE;
	if (!_skr_pipeline_cache.materials[material_idx].active)   return VK_NULL_HANDLE;

	return _skr_pipeline_cache.materials[material_idx].descriptor_layout;
}

VkRenderPass _skr_pipeline_get_renderpass(int32_t renderpass_idx) {
	if (renderpass_idx < 0 || renderpass_idx >= SKR_MAX_RENDERPASSES) return VK_NULL_HANDLE;
	if (!_skr_pipeline_cache.renderpasses[renderpass_idx].active)     return VK_NULL_HANDLE;

	return _skr_pipeline_cache.renderpasses[renderpass_idx].render_pass;
}

///////////////////////////////////////////////////////////////////////////////
// Internal helpers
///////////////////////////////////////////////////////////////////////////////

static const char* _skr_format_to_string(VkFormat format) {
	switch (format) {
		case VK_FORMAT_UNDEFINED:           return "none";
		case VK_FORMAT_B8G8R8A8_SRGB:       return "bgra8_srgb";
		case VK_FORMAT_B8G8R8A8_UNORM:      return "bgra8";
		case VK_FORMAT_R8G8B8A8_SRGB:       return "rgba8_srgb";
		case VK_FORMAT_R8G8B8A8_UNORM:      return "rgba8";
		case VK_FORMAT_R16G16B16A16_SFLOAT: return "rgba16f";
		case VK_FORMAT_R32G32B32A32_SFLOAT: return "rgba32f";
		case VK_FORMAT_D16_UNORM:           return "d16";
		case VK_FORMAT_D32_SFLOAT:          return "d32";
		case VK_FORMAT_D24_UNORM_S8_UINT:   return "d24s8";
		case VK_FORMAT_D16_UNORM_S8_UINT:   return "d16s8";
		case VK_FORMAT_D32_SFLOAT_S8_UINT:  return "d32s8";
		default:                            return "unknown";
	}
}

static VkRenderPass _skr_pipeline_create_renderpass(const skr_pipeline_renderpass_key_t* key) {
	VkAttachmentDescription attachments[3];
	uint32_t attachment_count = 0;

	bool use_msaa  = key->samples > VK_SAMPLE_COUNT_1_BIT && key->resolve_format != VK_FORMAT_UNDEFINED;
	bool has_color = key->color_format != VK_FORMAT_UNDEFINED;

	// Color attachment (MSAA if samples > 1) - only if we have a color format
	VkAttachmentReference color_ref = {0};
	if (has_color) {
		attachments[attachment_count] = (VkAttachmentDescription){
			.format         = key->color_format,
			.samples        = key->samples,
			.loadOp         = key->color_load_op,
			.storeOp        = use_msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};

		color_ref.attachment = attachment_count;
		color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachment_count++;
	}

	// Resolve attachment (for MSAA)
	VkAttachmentReference resolve_ref = {0};
	if (use_msaa) {
		attachments[attachment_count] = (VkAttachmentDescription){
			.format         = key->resolve_format,  // Use the actual resolve target format
			.samples        = VK_SAMPLE_COUNT_1_BIT,
			.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};
		resolve_ref.attachment = attachment_count;
		resolve_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachment_count++;
	}

	// Depth attachment (if present)
	VkAttachmentReference depth_ref = {0};
	if (key->depth_format != VK_FORMAT_UNDEFINED) {
		// Check if this is a depth+stencil format
		bool has_stencil = _skr_format_has_stencil(key->depth_format);

		attachments[attachment_count] = (VkAttachmentDescription){
			.format         = key->depth_format,
			.samples        = key->samples,
			.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp        = key->depth_store_op,  // Use the store op from the key (based on readable flag)
			.stencilLoadOp  = has_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = has_stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,  // Expect already transitioned
			.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		};

		depth_ref.attachment = attachment_count;
		depth_ref.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachment_count++;
	}

	// Subpass
	VkSubpassDescription subpass = {
		.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount    = has_color ? 1 : 0,
		.pColorAttachments       = has_color ? &color_ref : NULL,
		.pResolveAttachments     = use_msaa ? &resolve_ref : NULL,
		.pDepthStencilAttachment = key->depth_format != VK_FORMAT_UNDEFINED ? &depth_ref : NULL,
	};

	// Subpass dependencies
	VkSubpassDependency dependencies[2] = {
		{
			.srcSubpass    = VK_SUBPASS_EXTERNAL,
			.dstSubpass    = 0,
			.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		},
		{
			.srcSubpass    = VK_SUBPASS_EXTERNAL,
			.dstSubpass    = 0,
			.srcStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			.dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		},
	};

	VkRenderPassCreateInfo render_pass_info = {
		.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = attachment_count,
		.pAttachments    = attachments,
		.subpassCount    = 1,
		.pSubpasses      = &subpass,
		.dependencyCount = 2,
		.pDependencies   = dependencies,
	};

	VkRenderPass render_pass;
	if (vkCreateRenderPass(_skr_vk.device, &render_pass_info, NULL, &render_pass) != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to create render pass");
		return VK_NULL_HANDLE;
	}

	// Generate debug name based on render pass configuration
	char name[256];
	snprintf(name, sizeof(name), "rpass_");
	_skr_append_renderpass_config(name, sizeof(name), key);
	_skr_set_debug_name(VK_OBJECT_TYPE_RENDER_PASS, (uint64_t)render_pass, name);

	return render_pass;
}

static VkDescriptorSetLayout _skr_pipeline_create_descriptor_layout(const sksc_shader_meta_t* meta) {
	if (!meta || (meta->buffer_count == 0 && meta->resource_count == 0)) {
		return VK_NULL_HANDLE;
	}

	VkDescriptorSetLayoutBinding bindings[32];
	uint32_t binding_count = 0;

	// Add buffer bindings
	for (uint32_t i = 0; i < meta->buffer_count; i++) {
		skr_bind_t bind = meta->buffers[i].bind;

		VkShaderStageFlags stages = 0;
		if (bind.stage_bits & skr_stage_vertex ) stages |= VK_SHADER_STAGE_VERTEX_BIT;
		if (bind.stage_bits & skr_stage_pixel  ) stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
		if (bind.stage_bits & skr_stage_compute) stages |= VK_SHADER_STAGE_COMPUTE_BIT;

		bindings[binding_count++] = (VkDescriptorSetLayoutBinding){
			.binding            = meta->buffers[i].bind.slot,
			.descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount    = 1,
			.stageFlags         = stages,
			.pImmutableSamplers = NULL,
		};
	}

	// Add resource bindings (textures and storage buffers)
	for (uint32_t i = 0; i < meta->resource_count; i++) {
		VkDescriptorType desc_type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
		skr_bind_t bind = meta->resources[i].bind;

		// Determine descriptor type based on register type
		switch (bind.register_type) {
			case skr_register_constant:      desc_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         break;
			case skr_register_texture:       desc_type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
			case skr_register_read_buffer:   desc_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         break; // (StructuredBuffer)
			case skr_register_readwrite:     desc_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         break; // (RWStructuredBuffer)
			case skr_register_readwrite_tex: desc_type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          break; // (RWTexture)
			default:                         desc_type = VK_DESCRIPTOR_TYPE_MAX_ENUM; break;
		}

		VkShaderStageFlags stages = 0;
		if (bind.stage_bits & skr_stage_vertex ) stages |= VK_SHADER_STAGE_VERTEX_BIT;
		if (bind.stage_bits & skr_stage_pixel  ) stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
		if (bind.stage_bits & skr_stage_compute) stages |= VK_SHADER_STAGE_COMPUTE_BIT;

		bindings[binding_count++] = (VkDescriptorSetLayoutBinding){
			.binding            = bind.slot,
			.descriptorType     = desc_type,
			.descriptorCount    = 1,
			.stageFlags         = stages,
			.pImmutableSamplers = NULL,
		};
	}

	VkDescriptorSetLayoutCreateInfo layout_info = {
		.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
		.bindingCount = binding_count,
		.pBindings    = bindings,
	};

	VkDescriptorSetLayout layout;
	if (vkCreateDescriptorSetLayout(_skr_vk.device, &layout_info, NULL, &layout) != VK_SUCCESS) {
		skr_log(skr_log_warning, "Failed to create descriptor set layout");
		return VK_NULL_HANDLE;
	}

	return layout;
}

static VkPipelineLayout _skr_pipeline_create_layout(VkDescriptorSetLayout descriptor_layout) {
	VkPipelineLayoutCreateInfo layout_info = {
		.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = descriptor_layout != VK_NULL_HANDLE ? 1 : 0,
		.pSetLayouts    = descriptor_layout != VK_NULL_HANDLE ? &descriptor_layout : NULL,
	};

	VkPipelineLayout layout;
	if (vkCreatePipelineLayout(_skr_vk.device, &layout_info, NULL, &layout) != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to create pipeline layout");
		return VK_NULL_HANDLE;
	}

	// Pipeline layouts are created per-material, name will be set during material registration
	_skr_set_debug_name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)layout, "pipeline_layout");

	return layout;
}

static VkPipeline _skr_pipeline_create(int32_t material_idx, int32_t renderpass_idx, int32_t vertformat_idx) {
	const skr_material_info_t*           mat_info  = &_skr_pipeline_cache.materials   [material_idx  ].info;
	const skr_pipeline_renderpass_key_t* rp_key    = &_skr_pipeline_cache.renderpasses[renderpass_idx].key;
	const skr_vert_type_t*               vert_type = &_skr_pipeline_cache.vertformats [vertformat_idx].vert_type;
	const VkPipelineLayout               layout    =  _skr_pipeline_cache.materials   [material_idx  ].layout;
	const VkRenderPass                   rp        =  _skr_pipeline_cache.renderpasses[renderpass_idx].render_pass;

	// Shader stages
	VkPipelineShaderStageCreateInfo shader_stages[2];
	uint32_t stage_count = 0;

	if (mat_info->shader->vertex_stage.shader != VK_NULL_HANDLE) {
		shader_stages[stage_count++] = (VkPipelineShaderStageCreateInfo){
			.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage               = VK_SHADER_STAGE_VERTEX_BIT,
			.module              = mat_info->shader->vertex_stage.shader,
			.pName               = "vs",
			.pSpecializationInfo = NULL,
		};
	}

	if (mat_info->shader->pixel_stage.shader != VK_NULL_HANDLE) {
		shader_stages[stage_count++] = (VkPipelineShaderStageCreateInfo){
			.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage               = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module              = mat_info->shader->pixel_stage.shader,
			.pName               = "ps",
			.pSpecializationInfo = NULL,
		};
	}

	// Vertex input - baked from vertex type
	VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount   = vert_type ? 1 : 0,
		.pVertexBindingDescriptions      = vert_type ? &vert_type->binding : NULL,
		.vertexAttributeDescriptionCount = vert_type ? vert_type->component_count : 0,
		.pVertexAttributeDescriptions    = vert_type ? vert_type->attributes : NULL,
	};

	// Input assembly
	VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		.primitiveRestartEnable = VK_FALSE,
	};

	// Viewport state (dynamic)
	VkPipelineViewportStateCreateInfo viewport_state = {
		.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount  = 1,
	};

	// Rasterization
	VkPipelineRasterizationStateCreateInfo rasterizer = {
		.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.depthClampEnable        = VK_FALSE,
		.rasterizerDiscardEnable = VK_FALSE,
		.polygonMode             = VK_POLYGON_MODE_FILL,
		.cullMode                = _skr_to_vk_cull(mat_info->cull),
		.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.depthBiasEnable         = VK_FALSE,
		.lineWidth               = 1.0f,
	};

	// Multisampling
	VkPipelineMultisampleStateCreateInfo multisampling = {
		.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples  = rp_key->samples,
		.sampleShadingEnable   = VK_FALSE,
		.alphaToCoverageEnable = mat_info->alpha_to_coverage ? VK_TRUE : VK_FALSE,
	};

	// Depth/stencil
	bool stencil_enabled = (mat_info->write_mask & skr_write_stencil) ||
	                       mat_info->stencil_front.compare != skr_compare_none ||
	                       mat_info->stencil_back.compare != skr_compare_none;

	VkStencilOpState front_stencil = {
		.failOp      = _skr_to_vk_stencil_op(mat_info->stencil_front.fail_op),
		.passOp      = _skr_to_vk_stencil_op(mat_info->stencil_front.pass_op),
		.depthFailOp = _skr_to_vk_stencil_op(mat_info->stencil_front.depth_fail_op),
		.compareOp   = _skr_to_vk_compare(mat_info->stencil_front.compare),
		.compareMask = mat_info->stencil_front.compare_mask,
		.writeMask   = mat_info->stencil_front.write_mask,
		.reference   = mat_info->stencil_front.reference,
	};

	VkStencilOpState back_stencil = {
		.failOp      = _skr_to_vk_stencil_op(mat_info->stencil_back.fail_op),
		.passOp      = _skr_to_vk_stencil_op(mat_info->stencil_back.pass_op),
		.depthFailOp = _skr_to_vk_stencil_op(mat_info->stencil_back.depth_fail_op),
		.compareOp   = _skr_to_vk_compare(mat_info->stencil_back.compare),
		.compareMask = mat_info->stencil_back.compare_mask,
		.writeMask   = mat_info->stencil_back.write_mask,
		.reference   = mat_info->stencil_back.reference,
	};

	VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable       = mat_info->depth_test != skr_compare_none ? VK_TRUE : VK_FALSE,
		.depthWriteEnable      = (mat_info->write_mask & skr_write_depth) ? VK_TRUE : VK_FALSE,
		.depthCompareOp        = _skr_to_vk_compare(mat_info->depth_test),
		.depthBoundsTestEnable = VK_FALSE,
		.stencilTestEnable     = stencil_enabled ? VK_TRUE : VK_FALSE,
		.front                 = front_stencil,
		.back                  = back_stencil,
	};

	// Color blending - check if blend is enabled by seeing if any factors are non-zero
	// Zero-initialized blend state means "no blending" - pass source through unchanged
	bool blend_enabled = (mat_info->blend_state.src_color_factor != skr_blend_zero ||
	                      mat_info->blend_state.dst_color_factor != skr_blend_zero ||
	                      mat_info->blend_state.src_alpha_factor != skr_blend_zero ||
	                      mat_info->blend_state.dst_alpha_factor != skr_blend_zero);

	// When blend is disabled, always use ONE for src and ZERO for dst (pass through source)
	VkBlendFactor src_color = blend_enabled ? _skr_to_vk_blend_factor(mat_info->blend_state.src_color_factor) : VK_BLEND_FACTOR_ONE;
	VkBlendFactor dst_color = blend_enabled ? _skr_to_vk_blend_factor(mat_info->blend_state.dst_color_factor) : VK_BLEND_FACTOR_ZERO;
	VkBlendFactor src_alpha = blend_enabled ? _skr_to_vk_blend_factor(mat_info->blend_state.src_alpha_factor) : VK_BLEND_FACTOR_ONE;
	VkBlendFactor dst_alpha = blend_enabled ? _skr_to_vk_blend_factor(mat_info->blend_state.dst_alpha_factor) : VK_BLEND_FACTOR_ZERO;

	VkPipelineColorBlendAttachmentState color_blend_attachment = {
		.blendEnable         = blend_enabled ? VK_TRUE : VK_FALSE,
		.srcColorBlendFactor = src_color,
		.dstColorBlendFactor = dst_color,
		.colorBlendOp        = _skr_to_vk_blend_op(mat_info->blend_state.color_op),
		.srcAlphaBlendFactor = src_alpha,
		.dstAlphaBlendFactor = dst_alpha,
		.alphaBlendOp        = _skr_to_vk_blend_op(mat_info->blend_state.alpha_op),
		.colorWriteMask      =
			((mat_info->write_mask & skr_write_r) ? VK_COLOR_COMPONENT_R_BIT : 0) |
			((mat_info->write_mask & skr_write_g) ? VK_COLOR_COMPONENT_G_BIT : 0) |
			((mat_info->write_mask & skr_write_b) ? VK_COLOR_COMPONENT_B_BIT : 0) |
			((mat_info->write_mask & skr_write_a) ? VK_COLOR_COMPONENT_A_BIT : 0),
	};

	VkPipelineColorBlendStateCreateInfo color_blending = {
		.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.logicOpEnable   = VK_FALSE,
		.attachmentCount = 1,
		.pAttachments    = &color_blend_attachment,
	};

	// Dynamic state
	VkDynamicState dynamic_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};

	VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]),
		.pDynamicStates    = dynamic_states,
	};

	// Create pipeline
	VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount          = stage_count,
		.pStages             = shader_stages,
		.pVertexInputState   = &vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState      = &viewport_state,
		.pRasterizationState = &rasterizer,
		.pMultisampleState   = &multisampling,
		.pDepthStencilState  = &depth_stencil,
		.pColorBlendState    = &color_blending,
		.pDynamicState       = &dynamic_state,
		.layout              = layout,
		.renderPass          = rp,
		.subpass             = 0,
	};

	VkPipeline pipeline;
	VkResult   result = vkCreateGraphicsPipelines(_skr_vk.device, _skr_vk.pipeline_cache, 1, &pipeline_info, NULL, &pipeline);
	if (result != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to create graphics pipeline");
		return VK_NULL_HANDLE;
	}

	// Generate debug name based on all three pipeline dimensions: material + renderpass + vertex format
	char name[256];

	// Material dimension (shader + blend mode)
	const char* shader_name = (mat_info->shader->meta && mat_info->shader->meta->name[0])
		? mat_info->shader->meta->name
		: "shader";

	snprintf(name, sizeof(name), "pipeline_%s_(", shader_name);
	_skr_append_material_config  (name, sizeof(name), mat_info);
	strcat(name, ")_(");
	_skr_append_renderpass_config(name, sizeof(name), rp_key);
	strcat(name, ")_(");
	_skr_append_vertex_format    (name, sizeof(name), vert_type->components, vert_type->component_count);
	strcat(name, ")");
	_skr_set_debug_name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)pipeline, name);

	return pipeline;
}

///////////////////////////////////////////////////////////////////////////////
// Framebuffer creation
///////////////////////////////////////////////////////////////////////////////

VkFramebuffer _skr_create_framebuffer(VkRenderPass render_pass, skr_tex_t* color, skr_tex_t* depth, skr_tex_t* opt_resolve) {
	VkImageView attachments[3];
	uint32_t    attachment_count = 0;
	uint32_t    width            = 1;
	uint32_t    height           = 1;
	uint32_t    layers           = 1;

	if (color) {
		attachments[attachment_count++] = color->view;
		width                           = color->size.x;
		height                          = color->size.y;
		// For array textures, size.z contains the layer count
		if (color->flags & skr_tex_flags_array) {
			layers = color->size.z;
		}
	}

	// Resolve attachment comes after color but before depth
	if (opt_resolve && color && color->samples > VK_SAMPLE_COUNT_1_BIT) {
		attachments[attachment_count++] = opt_resolve->view;
	}

	if (depth) {
		attachments[attachment_count++] = depth->view;
		if (width == 1 && height == 1) {
			width  = depth->size.x;
			height = depth->size.y;
		}
		// Depth buffer should have same layer count as color
		if (depth->flags & skr_tex_flags_array) {
			layers = depth->size.z;
		}
	}

	VkFramebufferCreateInfo framebuffer_info = {
		.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass      = render_pass,
		.attachmentCount = attachment_count,
		.pAttachments    = attachments,
		.width           = width,
		.height          = height,
		.layers          = layers,
	};

	VkFramebuffer framebuffer;
	if (vkCreateFramebuffer(_skr_vk.device, &framebuffer_info, NULL, &framebuffer) != VK_SUCCESS) {
		skr_log(skr_log_critical, "Failed to create framebuffer");
		return VK_NULL_HANDLE;
	}

	return framebuffer;
}
