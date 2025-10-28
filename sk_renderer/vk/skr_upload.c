// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "_sk_renderer.h"
#include "../skr_log.h"

#include <assert.h>
#include <pthread.h>
#include <threads.h>
#include <string.h>
#include <stdlib.h>

///////////////////////////////////////////////////////////////////////////////

thread_local _skr_vk_thread_t _skr_thread;

///////////////////////////////////////////////////////////////////////////////

bool _skr_command_init() {
	skr_logf(skr_log_info, "Using %s queue (family %d)", _skr_vk.has_dedicated_transfer ? "transfer" : "graphics", _skr_vk.transfer_queue_family);

	memset(_skr_vk.thread_pools, 0, sizeof(_skr_vk.thread_pools));
	return true;
}

///////////////////////////////////////////////////////////////////////////////

void _skr_command_shutdown() {
	vkDeviceWaitIdle(_skr_vk.device);

	// Destroy thread command pools and per-thread command ring fences
	pthread_mutex_lock(&_skr_vk.thread_pool_mutex);
	for (uint32_t i = 0; i < _skr_vk.thread_pool_ct; i++) {
		for (uint32_t c = 0; c < skr_MAX_COMMAND_RING; c++) {
			// Execute and free any remaining destroy lists
			_skr_destroy_list_execute(&_skr_vk.thread_pools[i]->cmd_ring[c].destroy_list);
			_skr_destroy_list_free   (&_skr_vk.thread_pools[i]->cmd_ring[c].destroy_list);

			vkDestroyFence(_skr_vk.device, _skr_vk.thread_pools[i]->cmd_ring[c].fence, NULL);
		}

		if (_skr_vk.thread_pools[i]->cmd_pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(_skr_vk.device, _skr_vk.thread_pools[i]->cmd_pool, NULL);

		*_skr_vk.thread_pools[i] = (_skr_vk_thread_t){};
		_skr_vk.thread_pools [i] = NULL;
		_skr_vk.thread_pool_ct   = 0;
	}
	pthread_mutex_unlock(&_skr_vk.thread_pool_mutex);
}

///////////////////////////////////////////////////////////////////////////////

static _skr_vk_thread_t* _skr_command_get_thread() {
	if (!_skr_thread.alive) {
		if (_skr_vk.thread_pool_ct >= skr_MAX_THREAD_POOLS) {
			skr_logf(skr_log_critical, "Exceeded maximum thread pools (%d)", skr_MAX_THREAD_POOLS);
			return NULL;
		}

		skr_logf(skr_log_info, "Using thread #%d", _skr_vk.thread_pool_ct);

		// Set up data for this thread
		VkCommandPoolCreateInfo pool_info = {
			.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = _skr_vk.graphics_queue_family,  // Use graphics queue for main commands
		};
		if (vkCreateCommandPool(_skr_vk.device, &pool_info, NULL, &_skr_thread.cmd_pool) != VK_SUCCESS) {
			skr_log(skr_log_warning, "Failed to create thread command pool");
			return NULL;
		}
		_skr_thread.alive          = true;
		_skr_thread.active_cmd     = VK_NULL_HANDLE;
		_skr_thread.cmd_ring_index = 0;
		_skr_thread.ref_count      = 0;

		// Register it in a list, so we can clean it all up on shutdown
		pthread_mutex_lock(&_skr_vk.thread_pool_mutex);
		_skr_vk.thread_pools[_skr_vk.thread_pool_ct] = &_skr_thread;
		_skr_vk.thread_pool_ct++;
		pthread_mutex_unlock(&_skr_vk.thread_pool_mutex);
	}
	return &_skr_thread;
}

///////////////////////////////////////////////////////////////////////////////

static _skr_command_ring_slot_t *_skr_command_ring_begin(_skr_vk_thread_t* pool) {
	// Find available slot in the per-thread command ring
	_skr_command_ring_slot_t* slot      = NULL;
	uint32_t                  start_idx = pool->cmd_ring_index;

	for (uint32_t i = 0; i < skr_MAX_COMMAND_RING; i++) {
		uint32_t                  idx  = (start_idx + i) % skr_MAX_COMMAND_RING;
		_skr_command_ring_slot_t* curr = &pool->cmd_ring[idx];

		// Use this slot if available
		if (!curr->alive) {
			slot        = curr;
			slot->alive = true;
			pool->cmd_ring_index = (idx + 1) % skr_MAX_COMMAND_RING;
			break;
		}
	}

	// If no slots available, wait for oldest one
	if (!slot) {
		uint32_t                  idx  = start_idx;
		_skr_command_ring_slot_t* curr = &pool->cmd_ring[idx];

		vkWaitForFences(_skr_vk.device, 1, &curr->fence, VK_TRUE, UINT64_MAX);
		slot         = curr;
		slot->alive = true;
		pool->cmd_ring_index = (idx + 1) % skr_MAX_COMMAND_RING;
	}

	// Check fence and execute destroy list if fence is signaled
	if (slot->fence != VK_NULL_HANDLE) {
		VkResult fence_status = vkGetFenceStatus(_skr_vk.device, slot->fence);
		if (fence_status == VK_SUCCESS) {
			// Fence is signaled, execute the destroy list
			_skr_destroy_list_execute(&slot->destroy_list);
			_skr_destroy_list_clear(&slot->destroy_list);
		}
	}

	// Allocate command buffer if needed
	if (slot->cmd == VK_NULL_HANDLE) {
		vkAllocateCommandBuffers(_skr_vk.device, &(VkCommandBufferAllocateInfo){
			.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandPool        = pool->cmd_pool,
			.commandBufferCount = 1,
		}, &slot->cmd);
		vkCreateFence(_skr_vk.device, &(VkFenceCreateInfo){
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		}, NULL, &slot->fence);
		slot->destroy_list = _skr_destroy_list_create();
	} else {
		vkResetCommandBuffer(slot->cmd, 0);
		vkResetFences       (_skr_vk.device, 1, &slot->fence);
	}

	vkBeginCommandBuffer(slot->cmd, &(VkCommandBufferBeginInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	});

	return slot;
}

///////////////////////////////////////////////////////////////////////////////

_skr_command_context_t _skr_command_begin() {
	_skr_vk_thread_t* pool = _skr_command_get_thread();
	assert(pool);
	assert(pool->ref_count == 0 && "Ref count should be 0 at batch start");

	return _skr_command_acquire();
}

///////////////////////////////////////////////////////////////////////////////

bool _skr_command_try_get_active(_skr_command_context_t* out_ctx) {
	*out_ctx = (_skr_command_context_t){};

	_skr_vk_thread_t* pool = _skr_command_get_thread();
	assert(pool);
	
	if (pool->active_cmd) {
		*out_ctx = (_skr_command_context_t){
			.cmd          = pool->active_cmd->cmd,
			.destroy_list = &pool->active_cmd->destroy_list,
		};
		return true;
	}
	return false;
}

///////////////////////////////////////////////////////////////////////////////

_skr_command_context_t _skr_command_acquire() {
	_skr_vk_thread_t* pool = _skr_command_get_thread();
	assert(pool);

	if (pool->ref_count == 0)
		pool->active_cmd = _skr_command_ring_begin(pool);
	
	pool->ref_count++;
	return (_skr_command_context_t){
		.cmd          = pool->active_cmd->cmd,
		.destroy_list = &pool->active_cmd->destroy_list,
	};
}

///////////////////////////////////////////////////////////////////////////////

void _skr_command_release(VkCommandBuffer buffer) {
	_skr_vk_thread_t* pool = _skr_command_get_thread();
	assert(pool);

	pool->ref_count--;
	assert(pool->ref_count       >= 0      && "Unbalanced acquire/release");
	assert(pool->active_cmd->cmd == buffer && "Shouldn't release someone else's buffer!");

	if (pool->ref_count == 0) {
		// Outside a batch: submit the command buffer from the ring
		// The ring will handle waiting when it needs to reuse a slot
		vkEndCommandBuffer(pool->active_cmd->cmd);
		vkQueueSubmit(_skr_vk.graphics_queue, 1, &(VkSubmitInfo){
			.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers    = &pool->active_cmd->cmd,
		}, pool->active_cmd->fence);
		pool->active_cmd = NULL;
	}
}

///////////////////////////////////////////////////////////////////////////////

VkCommandBuffer _skr_command_end() {
	_skr_vk_thread_t* pool = _skr_command_get_thread();
	assert(pool);

	pool->ref_count--;
	assert(pool->ref_count == 0 && "Unbalanced acquire/release - ref count should be 0");

	return pool->active_cmd->cmd;
}

///////////////////////////////////////////////////////////////////////////////

void _skr_command_end_submit(const VkSemaphore* opt_wait_semaphore, const VkSemaphore* opt_signal_semaphore, VkFence* out_opt_fence) {
	_skr_vk_thread_t* pool = _skr_command_get_thread();
	assert(pool);

	pool->ref_count--;
	assert(pool->ref_count == 0 && "Unbalanced acquire/release - ref count should be 0");

	// End the command buffer
	vkEndCommandBuffer(pool->active_cmd->cmd);

	// Submit with optional semaphores and caller-provided fence
	// The caller is responsible for fence synchronization for batched operations
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	vkQueueSubmit(_skr_vk.graphics_queue, 1, &(VkSubmitInfo) {
		.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount   = 1,
		.pCommandBuffers      = &pool->active_cmd->cmd,
		.waitSemaphoreCount   = opt_wait_semaphore   ? 1 : 0,
		.pWaitSemaphores      = opt_wait_semaphore,
		.pWaitDstStageMask    = opt_wait_semaphore   ? &wait_stage : NULL,
		.signalSemaphoreCount = opt_signal_semaphore ? 1 : 0,
		.pSignalSemaphores    = opt_signal_semaphore,
	}, pool->active_cmd->fence);

	if (out_opt_fence) {
		*out_opt_fence = pool->active_cmd->fence;
	}
}

//TODO: all of this is just using the graphics_queue! It should be configurable