// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "_sk_renderer.h"
#include "skr_conversions.h"
#include "../skr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Surface
///////////////////////////////////////////////////////////////////////////////

// Helper to create/recreate swapchain and allocate resources
static bool _skr_surface_create_swapchain(skr_surface_t* surface, VkSwapchainKHR old_swapchain) {
	// Get surface capabilities
	VkSurfaceCapabilitiesKHR capabilities;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_skr_vk.physical_device, surface->surface, &capabilities);

	// Get surface formats
	uint32_t            format_count;
	VkSurfaceFormatKHR  formats[64];
	vkGetPhysicalDeviceSurfaceFormatsKHR(_skr_vk.physical_device, surface->surface, &format_count, NULL);
	vkGetPhysicalDeviceSurfaceFormatsKHR(_skr_vk.physical_device, surface->surface, &format_count, formats);

	// Choose format based on platform preference
	// Android/mobile: prefer RGBA for native GPU ordering
	// Desktop: prefer BGRA for Windows/D3D compositor compatibility
#ifdef __ANDROID__
	VkFormat preferred_formats[] = {
		VK_FORMAT_R8G8B8A8_SRGB,
		VK_FORMAT_B8G8R8A8_SRGB,
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_B8G8R8A8_UNORM,
	};
#else
	VkFormat preferred_formats[] = {
		VK_FORMAT_B8G8R8A8_SRGB,
		VK_FORMAT_R8G8B8A8_SRGB,
		VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_R8G8B8A8_UNORM,
	};
#endif

	VkSurfaceFormatKHR surface_format = formats[0];  // Fallback to first available
	for (uint32_t j = 0; j < sizeof(preferred_formats) / sizeof(preferred_formats[0]); j++) {
		for (uint32_t i = 0; i < format_count; i++) {
			if (formats[i].format == preferred_formats[j] &&
			    formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
				surface_format = formats[i];
				goto format_found;
			}
		}
	}
format_found:
	skr_logf(skr_log_info, "Selected surface format: %d (colorSpace: %d)", surface_format.format, surface_format.colorSpace);

	// Get present modes
	uint32_t         present_mode_count;
	VkPresentModeKHR present_modes[16];
	vkGetPhysicalDeviceSurfacePresentModesKHR(_skr_vk.physical_device, surface->surface, &present_mode_count, NULL);
	vkGetPhysicalDeviceSurfacePresentModesKHR(_skr_vk.physical_device, surface->surface, &present_mode_count, present_modes);

	// Choose present mode (FIFO guaranteed, FIFO_LATEST_READY if available)
	VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
	for (uint32_t i = 0; i < present_mode_count; i++) {
		if (present_modes[i] == 1000361000) { // VK_PRESENT_MODE_FIFO_LATEST_READY_EXT
			present_mode = present_modes[i];
			break;
		}
	}

	// Determine extent
	VkExtent2D extent = capabilities.currentExtent;
	if (extent.width == UINT32_MAX) {
		// If current extent is undefined, use a default size
		extent.width  = 1280;
		extent.height = 720;
	}

	// Handle minimized window (0x0 extent)
	if (extent.width == 0 || extent.height == 0) {
		return false;
	}

	// Determine image count
	uint32_t image_count = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
		image_count = capabilities.maxImageCount;
	}

	// Create swapchain
	VkSwapchainCreateInfoKHR swapchain_info = {
		.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface          = surface->surface,
		.minImageCount    = image_count,
		.imageFormat      = surface_format.format,
		.imageColorSpace  = surface_format.colorSpace,
		.imageExtent      = extent,
		.imageArrayLayers = 1,
		.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode      = present_mode,
		.clipped          = VK_TRUE,
		.oldSwapchain     = old_swapchain,
	};

	VkSwapchainKHR swapchain;
	VkResult vr = vkCreateSwapchainKHR(_skr_vk.device, &swapchain_info, NULL, &swapchain);
	SKR_VK_CHECK_RET(vr, "vkCreateSwapchainKHR", false);

	// Destroy old swapchain if provided
	if (old_swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(_skr_vk.device, old_swapchain, NULL);
	}
	surface->swapchain = swapchain;

	// Get swapchain images
	VkImage vk_images[16];
	uint32_t requested_count = image_count;
	vkGetSwapchainImagesKHR(_skr_vk.device, swapchain, &image_count, NULL);
	vkGetSwapchainImagesKHR(_skr_vk.device, swapchain, &image_count, vk_images);

	skr_logf(skr_log_info, "Swapchain created: requested %d, actual %d images", requested_count, image_count);

	// Reallocate images array and per-image semaphores if count changed
	if (image_count != surface->image_count) {
		// Destroy old per-image submit semaphores
		if (surface->semaphore_submit) {
			for (uint32_t i = 0; i < surface->image_count; i++) {
				if (surface->semaphore_submit[i]) vkDestroySemaphore(_skr_vk.device, surface->semaphore_submit[i], NULL);
			}
			free(surface->semaphore_submit);
		}

		// Allocate new per-image submit semaphores for new image count
		surface->semaphore_submit = (VkSemaphore*)calloc(image_count, sizeof(VkSemaphore));

		VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		for (uint32_t i = 0; i < image_count; i++) {
			vkCreateSemaphore(_skr_vk.device, &semaphore_info, NULL, &surface->semaphore_submit[i]);
		}

		// Reallocate images array
		if (surface->images) free(surface->images);
		surface->images      = (skr_tex_t*)calloc(image_count, sizeof(skr_tex_t));
		surface->image_count = image_count;
	}

	// Update size
	surface->size = (skr_vec2i_t){extent.width, extent.height};

	// Create image views and initialize layout tracking
	for (uint32_t i = 0; i < image_count; i++) {
		// Basic properties
		surface->images[i].image             = vk_images[i];
		surface->images[i].size              = (skr_vec3i_t){extent.width, extent.height, 1};
		surface->images[i].format            = _skr_from_vk_tex_fmt(surface_format.format);
		surface->images[i].samples           = VK_SAMPLE_COUNT_1_BIT;
		surface->images[i].mip_levels        = 1;
		surface->images[i].layer_count       = 1;
		surface->images[i].aspect_mask       = VK_IMAGE_ASPECT_COLOR_BIT;  // CRITICAL: Must be set!
		surface->images[i].framebuffer       = VK_NULL_HANDLE;
		surface->images[i].framebuffer_depth = VK_NULL_HANDLE;
		surface->images[i].framebuffer_pass  = VK_NULL_HANDLE;
		surface->images[i].sampler           = VK_NULL_HANDLE;
		surface->images[i].memory            = VK_NULL_HANDLE;  // Swapchain owns memory

		// Initialize layout tracking for swapchain images
		// Swapchain images start in UNDEFINED, render pass will transition them
		surface->images[i].current_layout       = VK_IMAGE_LAYOUT_UNDEFINED;
		surface->images[i].current_queue_family = _skr_vk.graphics_queue_family;
		surface->images[i].first_use            = true;
		surface->images[i].is_transient_discard = false;  // Swapchain images are not transient

		VkImageViewCreateInfo view_info = {
			.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image      = vk_images[i],
			.viewType   = VK_IMAGE_VIEW_TYPE_2D,
			.format     = surface_format.format,
			.components = {0},  // Defaults to IDENTITY
			.subresourceRange = {
				.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel   = 0,
				.levelCount     = 1,
				.baseArrayLayer = 0,
				.layerCount     = 1,
			},
		};

		vr = vkCreateImageView(_skr_vk.device, &view_info, NULL, &surface->images[i].view);
		SKR_VK_CHECK_NRET(vr, "vkCreateImageView");
	}

	return true;
}

skr_err_ skr_surface_create(void* vk_surface_khr, skr_surface_t* out_surface) {
	if (!out_surface) return skr_err_invalid_parameter;

	// Zero out immediately
	memset(out_surface, 0, sizeof(skr_surface_t));

	VkSurfaceKHR vk_surface = (VkSurfaceKHR)vk_surface_khr;
	if (!vk_surface) return skr_err_invalid_parameter;

	// Check present support
	VkBool32 present_support = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(_skr_vk.physical_device, _skr_vk.present_queue_family, vk_surface, &present_support);
	if (!present_support) {
		skr_log(skr_log_critical, "Surface doesn't support presentation");
		vkDestroySurfaceKHR(_skr_vk.instance, vk_surface, NULL);
		return skr_err_unsupported;
	}

	out_surface->surface = vk_surface;

	// Create swapchain using helper
	if (!_skr_surface_create_swapchain(out_surface, VK_NULL_HANDLE)) {
		vkDestroySurfaceKHR(_skr_vk.instance, vk_surface, NULL);
		memset(out_surface, 0, sizeof(skr_surface_t));
		return skr_err_device_error;
	}

	// Create per-frame synchronization objects (fences and acquire semaphores)
	VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	for (uint32_t i = 0; i < SKR_MAX_FRAMES_IN_FLIGHT; i++) {
		vkCreateSemaphore(_skr_vk.device, &semaphore_info, NULL, &out_surface->semaphore_acquire[i]);
	}

	return skr_err_success;
}

void skr_surface_destroy(skr_surface_t* surface) {
	if (!surface) return;

	//vkDeviceWaitIdle(_skr_vk.device);

	// Destroy per-frame synchronization objects
	for (uint32_t i = 0; i < SKR_MAX_FRAMES_IN_FLIGHT; i++)
		_skr_command_destroy_semaphore(NULL, surface->semaphore_acquire[i]);

	// Destroy per-image synchronization objects
	if (surface->semaphore_submit) {
		for (uint32_t i = 0; i < surface->image_count; i++)
			_skr_command_destroy_semaphore(NULL, surface->semaphore_submit[i]);
		free(surface->semaphore_submit);
	}

	// Destroy image views and cached framebuffers
	if (surface->images) {
		for (uint32_t i = 0; i < surface->image_count; i++) {
			_skr_command_destroy_framebuffer(NULL, surface->images[i].framebuffer);
			_skr_command_destroy_framebuffer(NULL, surface->images[i].framebuffer_depth);
			_skr_command_destroy_image_view (NULL, surface->images[i].view);
		}
		free(surface->images);
	}

	_skr_command_destroy_surface  (NULL, surface->surface  );
	_skr_command_destroy_swapchain(NULL, surface->swapchain);
}

void skr_surface_resize(skr_surface_t* surface) {
	if (!surface) return;

	vkDeviceWaitIdle(_skr_vk.device);

	// Destroy old image views and framebuffers
	for (uint32_t i = 0; i < surface->image_count; i++) {
		skr_tex_t* tex = &surface->images[i];
		if (tex->framebuffer      ) { vkDestroyFramebuffer(_skr_vk.device, tex->framebuffer,       NULL); }
		if (tex->framebuffer_depth) { vkDestroyFramebuffer(_skr_vk.device, tex->framebuffer_depth, NULL); }
		if (tex->view             ) { vkDestroyImageView  (_skr_vk.device, tex->view,              NULL); }
	}

	// Recreate swapchain using helper (old swapchain will be destroyed by helper)
	_skr_surface_create_swapchain(surface, surface->swapchain);
}

skr_acquire_ skr_surface_next_tex(skr_surface_t* surface, skr_tex_t** out_tex) {
	if (!surface || !out_tex) return skr_acquire_error;

	*out_tex = NULL;

	if (surface->fence_frame[surface->frame_idx] != VK_NULL_HANDLE) {
		vkWaitForFences(_skr_vk.device, 1, &surface->fence_frame[surface->frame_idx], VK_TRUE, UINT64_MAX);
	}

	// Acquire next image using per-frame acquire semaphore
	// Frame fence ensures this semaphore is not in use from previous frames
	VkResult result = vkAcquireNextImageKHR(
		_skr_vk.device, surface->swapchain, UINT64_MAX,
		surface->semaphore_acquire[surface->frame_idx],
		VK_NULL_HANDLE, &surface->current_image
	);

	// Handle surface lost - cannot recover here, caller must recreate surface
	if (result == VK_ERROR_SURFACE_LOST_KHR) {
		skr_log(skr_log_critical, "Surface lost - full surface recreation needed");
		// Advance frame index since we won't call present() for this frame
		//surface->frame_idx = (surface->frame_idx + 1) % SKR_MAX_FRAMES_IN_FLIGHT;
		return skr_acquire_surface_lost;
	}

	// Handle swapchain out-of-date or suboptimal
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
		skr_log(skr_log_info, "Swapchain out of date - needs resize");

		// If VK_SUBOPTIMAL_KHR, the semaphore was signaled even though we won't use the image
		// We need to consume the semaphore with a dummy submit to unsignal it
		if (result == VK_SUBOPTIMAL_KHR) {
			VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			vkQueueSubmit(_skr_vk.graphics_queue, 1, &(VkSubmitInfo){
				.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.waitSemaphoreCount   = 1,
				.pWaitSemaphores      = &surface->semaphore_acquire[surface->frame_idx],
				.pWaitDstStageMask    = &wait_stage,
				.commandBufferCount   = 0,  // No commands, just consume the semaphore
			}, VK_NULL_HANDLE);

			// Wait for the dummy submit to complete so semaphore is unsignaled
			vkQueueWaitIdle(_skr_vk.graphics_queue);
		}

		// Don't advance frame index - we can reuse the same (now unsignaled) semaphore
		return skr_acquire_needs_resize;
	}

	// Handle other errors
	if (result != VK_SUCCESS) {
		skr_logf(skr_log_critical, "Failed to acquire swapchain image: %d", result);
		return skr_acquire_error;
	}

	*out_tex = &surface->images[surface->current_image];
	return skr_acquire_success;
}

void skr_surface_present(skr_surface_t* surface) {
	if (!surface) return;

	// Transition swapchain image to PRESENT_SRC_KHR layout before presenting
	VkCommandBuffer cmd = _skr_command_acquire().cmd;
	skr_tex_t* swapchain_image = &surface->images[surface->current_image];
	_skr_tex_transition(cmd, swapchain_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);

	// Write end timestamp before ending command buffer
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _skr_vk.timestamp_pool, _skr_vk.flight_idx * 2 + 1);
	_skr_command_release(cmd);

	// End and submit the command buffer with semaphores for presentation
	_skr_command_end_submit(
		&surface->semaphore_acquire[surface->frame_idx],       // Wait on acquire semaphore
		&surface->semaphore_submit [surface->current_image],    // Signal submit semaphore
		&surface->fence_frame      [surface->frame_idx]
	);

	// Present: wait on per-image render semaphore
	vkQueuePresentKHR(_skr_vk.present_queue, &(VkPresentInfoKHR){
		.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores    = &surface->semaphore_submit[surface->current_image],  // Per-image!
		.swapchainCount     = 1,
		.pSwapchains        = &surface->swapchain,
		.pImageIndices      = &surface->current_image,
	});

	surface->frame_idx = (surface->frame_idx + 1) % SKR_MAX_FRAMES_IN_FLIGHT;
}

skr_vec2i_t skr_surface_get_size(const skr_surface_t* surface) {
	return surface ? surface->size : (skr_vec2i_t){0, 0};
}
