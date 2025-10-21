#include "../include/sk_renderer.h"
#include "skr_vulkan.h"
#include "../skr_log.h"

#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////

skr_render_list_t skr_render_list_create() {
	skr_render_list_t list = (skr_render_list_t){};
	list.capacity               = 16;
	list.items                  = malloc(sizeof(skr_render_item_t) * list.capacity);
	list.instance_data_capacity = 1024;
	list.instance_data          = malloc(list.instance_data_capacity);
	if (!list.items || !list.instance_data) {
		skr_log(skr_log_critical, "Failed to allocate render list");
		list = (skr_render_list_t){};
		return list;
	}

	// System buffer will be lazily created in skr_renderer_draw() when needed
	list.system_buffer_valid = false;

	return list;
}

void skr_render_list_destroy(skr_render_list_t* list) {
	if (!list) return;

	if (list->instance_buffer_valid) {
		skr_buffer_destroy(&list->instance_buffer);
	}
	if (list->system_buffer_valid) {
		skr_buffer_destroy(&list->system_buffer);
	}
	free(list->instance_data);
	free(list->items);
	*list = (skr_render_list_t){};
}

void skr_render_list_clear(skr_render_list_t* list) {
	if (!list) return;
	list->count = 0;
	list->instance_data_used = 0;
	list->needs_sort = false;
}

void skr_render_list_add(skr_render_list_t* list, skr_mesh_t* mesh, skr_material_t* material, const void* opt_instance_data, uint32_t instance_data_size, uint32_t instance_count) {
	if (!list || !mesh || !material) return;

	// Grow if needed
	if (list->count >= list->capacity) {
		uint32_t           new_capacity = list->capacity * 2;
		skr_render_item_t* new_items    = realloc(list->items, sizeof(skr_render_item_t) * new_capacity);
		if (!new_items) {
			skr_log(skr_log_critical, "Failed to grow render list");
			return;
		}
		list->items    = new_items;
		list->capacity = new_capacity;
	}

	// Add item
	skr_render_item_t* item = &list->items[list->count++];
	item->mesh               = mesh;
	item->material           = material;
	item->instance_offset    = list->instance_data_used;
	item->instance_data_size = instance_data_size;
	item->instance_count     = instance_count;

	// Copy instance data if provided
	uint32_t total_size = instance_data_size * instance_count;
	if (opt_instance_data && total_size > 0) {
		// Resize instance data if needed
		while (list->instance_data_used + total_size > list->instance_data_capacity) {
			uint32_t new_capacity = list->instance_data_capacity * 2;
			uint8_t* new_data     = realloc(list->instance_data, new_capacity);
			if (!new_data) {
				skr_log(skr_log_critical, "Failed to grow render list instance data");
				return;
			}
			list->instance_data          = new_data;
			list->instance_data_capacity = new_capacity;
		}
		memcpy(&list->instance_data[list->instance_data_used], opt_instance_data, total_size);
		list->instance_data_used += total_size;
	}

	// Mark list as needing sort
	list->needs_sort = true;
}

static int _skr_render_item_compare(const void* a, const void* b) {
	const skr_render_item_t* item_a = (const skr_render_item_t*)a;
	const skr_render_item_t* item_b = (const skr_render_item_t*)b;

	// Sort by queue offset first (allows explicit draw order control)
	int32_t queue_a = item_a->material->info.queue_offset;
	int32_t queue_b = item_b->material->info.queue_offset;
	if (queue_a < queue_b) return -1;
	if (queue_a > queue_b) return  1;

	// Then by mesh (instancing batches)
	if (item_a->mesh < item_b->mesh) return -1;
	if (item_a->mesh > item_b->mesh) return  1;

	// Then by material
	if (item_a->material < item_b->material) return -1;
	if (item_a->material > item_b->material) return  1;

	return 0;
}

void _skr_render_list_sort(skr_render_list_t* list) {
	if (!list || !list->needs_sort || list->count == 0) return;

	qsort(list->items, list->count, sizeof(skr_render_item_t), _skr_render_item_compare);
	list->needs_sort = false;
}
