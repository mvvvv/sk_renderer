#include "../include/sk_renderer.h"
#include "skr_vulkan.h"
#include "../skr_log.h"
#include "skr_conversions.h"
#include "skr_pipeline.h"
#include "_sk_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Vertex type creation
///////////////////////////////////////////////////////////////////////////////

skr_vert_type_t skr_vert_type_create(const skr_vert_component_t* items, int32_t item_count) {
	skr_vert_type_t result = {0};

	if (!items || item_count == 0) {
		skr_log(skr_log_warning, "Cannot create vertex type with no components");
		return result;
	}

	// Allocate storage
	result.component_count = item_count;
	result.components      = malloc(sizeof(skr_vert_component_t) * item_count);
	result.attributes      = malloc(sizeof(VkVertexInputAttributeDescription) * item_count);
	memcpy(result.components, items, sizeof(skr_vert_component_t) * item_count);

	// Calculate stride and populate attributes
	uint32_t offset = 0;
	for (int32_t i = 0; i < item_count; i++) {
		uint32_t component_size = _skr_vert_fmt_to_size(items[i].format) * items[i].count;

		result.attributes[i] = (VkVertexInputAttributeDescription){
			.location = i,
			.binding  = 0,
			.format   = _skr_to_vk_vert_fmt(items[i].format, items[i].count),
			.offset   = offset,
		};

		offset += component_size;
	}

	// Set up binding description
	result.binding = (VkVertexInputBindingDescription){
		.binding   = 0,
		.stride    = offset,
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};

	// Register with pipeline system (vertex format owns this registration)
	result.pipeline_idx = _skr_pipeline_register_vertformat(result);

	return result;
}

bool skr_vert_type_is_valid(const skr_vert_component_t* type) {
	const skr_vert_type_t* vert_type = (const skr_vert_type_t*)type;
	return vert_type && vert_type->attributes != NULL && vert_type->component_count > 0;
}

void skr_vert_type_destroy(skr_vert_type_t* type) {
	if (!type) return;

	// Unregister from pipeline system
	if (type->pipeline_idx >= 0) {
		_skr_pipeline_unregister_vertformat(type->pipeline_idx);
	}

	free(type->attributes);
	free(type->components);
	memset(type, 0, sizeof(skr_vert_type_t));
}

///////////////////////////////////////////////////////////////////////////////
// Mesh creation
///////////////////////////////////////////////////////////////////////////////

skr_mesh_t skr_mesh_create(const skr_vert_type_t* vert_type, skr_index_fmt_ ind_type, const void* vert_data, uint32_t vert_count, const void* opt_ind_data, uint32_t ind_count) {
	skr_mesh_t mesh = {0};

	// Store counts
	mesh.vert_count = vert_count;
	mesh.ind_count  = ind_count;
	mesh.ind_format = ind_type;
	mesh.vert_type  = vert_type;

	// Create vertex buffer if data provided
	if (vert_data && vert_count > 0) {
		mesh.vertex_buffer = skr_buffer_create(vert_data, vert_count, vert_type->binding.stride, skr_buffer_type_vertex, skr_use_static);

		if (!skr_buffer_is_valid(&mesh.vertex_buffer)) {
			skr_log(skr_log_critical, "Failed to create vertex buffer for mesh");
			skr_mesh_destroy(&mesh);
			return mesh;
		}
	}

	// Create index buffer if provided
	if (opt_ind_data && ind_count > 0) {
		mesh.ind_format_vk = _skr_to_vk_index_fmt(ind_type);
		uint32_t ind_stride = _skr_index_fmt_to_size(ind_type);
		mesh.index_buffer = skr_buffer_create(opt_ind_data, ind_count, ind_stride, skr_buffer_type_index, skr_use_static);

		if (!skr_buffer_is_valid(&mesh.index_buffer)) {
			skr_log(skr_log_critical, "Failed to create index buffer for mesh");
			skr_mesh_destroy(&mesh);
			return mesh;
		}
	}

	return mesh;
}

bool skr_mesh_is_valid(const skr_mesh_t* mesh) {
	return mesh && (skr_buffer_is_valid(&mesh->vertex_buffer) || mesh->ind_count > 0);
}

void skr_mesh_destroy(skr_mesh_t* mesh) {
	if (!mesh) return;

	skr_buffer_destroy(&mesh->vertex_buffer);
	skr_buffer_destroy(&mesh->index_buffer );
	*mesh = (skr_mesh_t){};
}

uint32_t skr_mesh_get_vert_count(const skr_mesh_t* mesh) {
	return mesh ? mesh->vert_count : 0;
}

uint32_t skr_mesh_get_ind_count(const skr_mesh_t* mesh) {
	return mesh ? mesh->ind_count : 0;
}

void skr_mesh_set_name(skr_mesh_t* mesh, const char* name) {
	if (!mesh) return;

	// Name the vertex and index buffers with appropriate suffixes
	char buffer_name[256];
	if (skr_buffer_is_valid(&mesh->vertex_buffer)) {
		snprintf(buffer_name, sizeof(buffer_name), "verts_%s", name);
		skr_buffer_set_name(&mesh->vertex_buffer, buffer_name);
	}
	if (skr_buffer_is_valid(&mesh->index_buffer)) {
		snprintf(buffer_name, sizeof(buffer_name), "indices_%s", name);
		skr_buffer_set_name(&mesh->index_buffer, buffer_name);
	}
}
