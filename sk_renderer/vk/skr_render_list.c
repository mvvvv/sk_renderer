// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "sk_renderer.h"
#include "_sk_renderer.h"

#include "skr_vulkan.h"

#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////

skr_err_ skr_render_list_create(skr_render_list_t* out_list) {
	if (!out_list) return skr_err_invalid_parameter;

	// Zero out immediately
	*out_list = (skr_render_list_t){};

	out_list->capacity                       = 16;
	out_list->items                          = _skr_malloc(sizeof(skr_render_item_t) * out_list->capacity);
	out_list->instance_data_capacity         = 1024;
	out_list->instance_data                  = _skr_malloc(out_list->instance_data_capacity);
	out_list->instance_data_sorted_capacity  = 1024;
	out_list->instance_data_sorted           = _skr_malloc(out_list->instance_data_sorted_capacity);
	out_list->material_data_capacity         = 1024;
	out_list->material_data                  = _skr_malloc(out_list->material_data_capacity);

	if (!out_list->items || !out_list->instance_data || !out_list->instance_data_sorted || !out_list->material_data) {
		skr_log(skr_log_critical, "Failed to allocate render list");
		_skr_free(out_list->items);
		_skr_free(out_list->instance_data);
		_skr_free(out_list->instance_data_sorted);
		_skr_free(out_list->material_data);
		*out_list = (skr_render_list_t){};
		return skr_err_out_of_memory;
	}

	// Buffers will be lazily created in skr_renderer_draw() when needed
	out_list->system_buffer_valid = false;
	out_list->instance_buffer_valid = false;
	out_list->material_param_buffer_valid = false;

	return skr_err_success;
}

void skr_render_list_destroy(skr_render_list_t* ref_list) {
	if (!ref_list) return;

	if (ref_list->instance_buffer_valid) {
		skr_buffer_destroy(&ref_list->instance_buffer);
	}
	if (ref_list->material_param_buffer_valid) {
		skr_buffer_destroy(&ref_list->material_param_buffer);
	}
	if (ref_list->system_buffer_valid) {
		skr_buffer_destroy(&ref_list->system_buffer);
	}
	_skr_free(ref_list->instance_data);
	_skr_free(ref_list->instance_data_sorted);
	_skr_free(ref_list->material_data);
	_skr_free(ref_list->items);
	*ref_list = (skr_render_list_t){};
}

void skr_render_list_clear(skr_render_list_t* ref_list) {
	if (!ref_list) return;
	ref_list->count = 0;
	ref_list->instance_data_used = 0;
	ref_list->material_data_used = 0;
	ref_list->needs_sort = false;
}

void skr_render_list_add_indexed(skr_render_list_t* ref_list, skr_mesh_t* mesh, skr_material_t* material, int32_t first_index, int32_t index_count, int32_t vertex_offset, const void* opt_instance_data, uint32_t single_instance_data_size, uint32_t instance_count) {
	if (!ref_list || !mesh || !material) return;

	// Grow if needed
	if (ref_list->count >= ref_list->capacity) {
		uint32_t           new_capacity = ref_list->capacity * 2;
		skr_render_item_t* new_items    = _skr_realloc(ref_list->items, sizeof(skr_render_item_t) * new_capacity);
		if (!new_items) {
			skr_log(skr_log_critical, "Failed to grow render list");
			return;
		}
		ref_list->items    = new_items;
		ref_list->capacity = new_capacity;
	}

	// Add item
	skr_render_item_t* item = &ref_list->items[ref_list->count++];
	item->mesh               = mesh;
	item->material           = material;
	item->instance_offset    = ref_list->instance_data_used;
	item->instance_data_size = single_instance_data_size;
	item->instance_count     = instance_count;
	item->first_index        = first_index;
	item->index_count        = index_count;
	item->vertex_offset      = vertex_offset;

	// Copy instance data if provided
	uint32_t total_size = single_instance_data_size * instance_count;
	if (opt_instance_data && total_size > 0) {
		// Resize instance data if needed
		while (ref_list->instance_data_used + total_size > ref_list->instance_data_capacity) {
			uint32_t new_capacity = ref_list->instance_data_capacity * 2;
			uint8_t* new_data     = _skr_realloc(ref_list->instance_data, new_capacity);
			if (!new_data) {
				skr_log(skr_log_critical, "Failed to grow render list instance data");
				return;
			}
			ref_list->instance_data          = new_data;
			ref_list->instance_data_capacity = new_capacity;
		}
		memcpy(&ref_list->instance_data[ref_list->instance_data_used], opt_instance_data, total_size);
		ref_list->instance_data_used += total_size;
	}

	// Mark list as needing sort
	ref_list->needs_sort = true;
}

void skr_render_list_add(skr_render_list_t* ref_list, skr_mesh_t* mesh, skr_material_t* material, const void* opt_instance_data, uint32_t single_instance_data_size, uint32_t instance_count) {
	// Call indexed version with default offsets (draw entire mesh)
	skr_render_list_add_indexed(ref_list, mesh, material, 0, 0, 0, opt_instance_data, single_instance_data_size, instance_count);
}

static int _skr_render_item_compare(const void* a, const void* b) {
	const skr_render_item_t* item_a = (const skr_render_item_t*)a;
	const skr_render_item_t* item_b = (const skr_render_item_t*)b;

	// Sort by queue offset first (allows explicit draw order control)
	int32_t queue_a = item_a->material->queue_offset;
	int32_t queue_b = item_b->material->queue_offset;
	if (queue_a < queue_b) return -1;
	if (queue_a > queue_b) return  1;

	// Then by mesh (instancing batches)
	if (item_a->mesh < item_b->mesh) return -1;
	if (item_a->mesh > item_b->mesh) return  1;

	// Then by material
	if (item_a->material < item_b->material) return -1;
	if (item_a->material > item_b->material) return  1;

	// Then by draw parameters (first_index, index_count, vertex_offset)
	// Items with different draw parameters can't be batched together
	if (item_a->first_index < item_b->first_index) return -1;
	if (item_a->first_index > item_b->first_index) return  1;

	if (item_a->index_count < item_b->index_count) return -1;
	if (item_a->index_count > item_b->index_count) return  1;

	if (item_a->vertex_offset < item_b->vertex_offset) return -1;
	if (item_a->vertex_offset > item_b->vertex_offset) return  1;

	return 0;
}

void _skr_render_list_sort(skr_render_list_t* ref_list) {
	if (!ref_list || !ref_list->needs_sort || ref_list->count == 0) return;

	qsort(ref_list->items, ref_list->count, sizeof(skr_render_item_t), _skr_render_item_compare);
	ref_list->needs_sort = false;

	// After sorting, instance_offset values no longer match the sorted order
	// Rebuild instance data in sorted order
	if (ref_list->instance_data_used > 0) {
		// Keep sorted buffer same size as instance_data buffer
		if (ref_list->instance_data_sorted_capacity != ref_list->instance_data_capacity) {
			_skr_free(ref_list->instance_data_sorted);
			ref_list->instance_data_sorted          = _skr_malloc(ref_list->instance_data_capacity);
			ref_list->instance_data_sorted_capacity = ref_list->instance_data_capacity;
			if (!ref_list->instance_data_sorted) {
				skr_log(skr_log_critical, "Failed to allocate render list sorted instance data");
				ref_list->instance_data_sorted_capacity = 0;
				return;
			}
		}

		// Copy instance data in sorted order and update offsets
		// Batch consecutive runs to minimize memcpy calls
		uint32_t sorted_offset = 0;
		uint32_t i = 0;
		while (i < ref_list->count) {
			skr_render_item_t* item = &ref_list->items[i];
			uint32_t           size = item->instance_data_size * item->instance_count;

			if (size > 0) {
				// Find run of consecutive items in source buffer
				uint32_t run_start_src = item->instance_offset;
				uint32_t run_start_dst = sorted_offset;
				uint32_t run_size      = size;
				uint32_t run_items     = 1;

				item->instance_offset = sorted_offset;
				sorted_offset += size;

				// Check if next items are consecutive in source
				while (i + run_items < ref_list->count) {
					skr_render_item_t* next_item = &ref_list->items[i + run_items];
					uint32_t           next_size = next_item->instance_data_size * next_item->instance_count;

					if (next_size > 0 && next_item->instance_offset == run_start_src + run_size) {
						next_item->instance_offset = sorted_offset;
						sorted_offset += next_size;
						run_size      += next_size;
						run_items++;
					} else {
						break;
					}
				}

				// Copy the entire run at once
				memcpy(&ref_list->instance_data_sorted[run_start_dst], &ref_list->instance_data[run_start_src], run_size);

				i += run_items;
			} else {
				i++;
			}
		}

		// Swap the buffers
		uint8_t* temp = ref_list->instance_data;
		ref_list->instance_data        = ref_list->instance_data_sorted;
		ref_list->instance_data_sorted = temp;
	}
}
