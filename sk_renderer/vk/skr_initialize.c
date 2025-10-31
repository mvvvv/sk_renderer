// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "_sk_renderer.h"
#include "skr_pipeline.h"
#include "skr_conversions.h"
#include "../skr_log.h"

#define VOLK_IMPLEMENTATION
#include <volk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Global state
///////////////////////////////////////////////////////////////////////////////

_skr_vk_t _skr_vk;

///////////////////////////////////////////////////////////////////////////////
// Validation layers
///////////////////////////////////////////////////////////////////////////////

static VKAPI_ATTR VkBool32 VKAPI_CALL _skr_vk_debug_callback(
	VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
	VkDebugUtilsMessageTypeFlagsEXT             type,
	const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
	void*                                       user_data) {

	if (callback_data->messageIdNumber == -1744492148) return VK_FALSE; // vkCreateGraphicsPipelines: pCreateInfos[] Inside the fragment shader, it writes to output Location X but there is no VkSubpassDescription::pColorAttachments[X] and this write is unused. Spec information at https://docs.vulkan.org/spec/latest/chapters/interfaces.html#interfaces-fragmentoutput
	if (callback_data->messageIdNumber == -937765618 ) return VK_FALSE; // vkCreateGraphicsPipelines: pCreateInfos[].pVertexInputState Vertex attribute at location X not consumed by shader.
	if (callback_data->messageIdNumber == -60244330  ) return VK_FALSE;
	if (callback_data->messageIdNumber ==  533026821 ) return VK_FALSE; // gl_Layer ?
	if (callback_data->messageIdNumber ==  115483881 ) return VK_FALSE; // Geometry shader req, might need attention

	const char *severity_str = severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT ? "VERBOSE" :
							   severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT    ? "INFO"    :
							   severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ? "WARNING" :
							   severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT   ? "ERROR"   : "UNKNOWN";

	printf("[Vulkan:%s] %s\n", severity_str, callback_data->pMessage);

	if (severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		severity_str = severity_str;
	}
	return VK_FALSE;
}

static bool _skr_vk_create_debug_messenger() {
	VkDebugUtilsMessengerCreateInfoEXT create_info = {
		.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
						   VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
						   VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
						   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
						   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
						   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
		.pfnUserCallback = _skr_vk_debug_callback,
	};

	VkResult vr = vkCreateDebugUtilsMessengerEXT(_skr_vk.instance, &create_info, NULL, &_skr_vk.debug_messenger);
	SKR_VK_CHECK_RET(vr, "vkCreateDebugUtilsMessengerEXT", false);

	_skr_cmd_destroy_debug_messenger(&_skr_vk.destroy_list, _skr_vk.debug_messenger);
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// Initialization
///////////////////////////////////////////////////////////////////////////////

bool skr_init(skr_settings_t settings) {
	if (_skr_vk.initialized) {
		skr_log(skr_log_warning, "sk_renderer already initialized");
		return false;
	}

	memset(&_skr_vk, 0, sizeof(_skr_vk));
	_skr_vk.validation_enabled        = settings.enable_validation;
	_skr_vk.current_renderpass_idx    = -1;
	_skr_vk.main_thread_id            = pthread_self();
	_skr_vk.destroy_list              = _skr_destroy_list_create();

	// Initialize volk
	VkResult vr = volkInitialize();
	SKR_VK_CHECK_RET(vr, volkInitialize, false);

	// Create instance
	VkApplicationInfo app_info = {
		.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName   = settings.app_name ? settings.app_name : "sk_renderer_app",
		.applicationVersion = settings.app_version,
		.pEngineName        = "sk_renderer",
		.engineVersion      = VK_MAKE_VERSION(0, 1, 0),
		.apiVersion         = VK_API_VERSION_1_1,
	};

	// Build list of desired extensions
	const char* desired_extensions[32];
	uint32_t    desired_extension_count = 0;
	for (uint32_t i = 0; i < settings.required_extension_count && i < 32; i++) {
		desired_extensions[desired_extension_count++] = settings.required_extensions[i];
	}
	if (_skr_vk.validation_enabled && desired_extension_count < 32) {
		desired_extensions[desired_extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	}
	if (desired_extension_count < 32) {
		desired_extensions[desired_extension_count++] = "VK_EXT_present_mode_fifo_latest_ready";
	}

	// Get available extensions
	uint32_t available_ext_count = 0;
	vkEnumerateInstanceExtensionProperties(NULL, &available_ext_count, NULL);
	VkExtensionProperties* available_exts = malloc(available_ext_count * sizeof(VkExtensionProperties));
	vkEnumerateInstanceExtensionProperties(NULL, &available_ext_count, available_exts);

	// Filter extensions to only those available
	const char* extensions[32];
	uint32_t    extension_count = 0;
	for (uint32_t i = 0; i < desired_extension_count; i++) {
		bool found = false;
		for (uint32_t j = 0; j < available_ext_count; j++) {
			if (strcmp(desired_extensions[i], available_exts[j].extensionName) == 0) {
				found = true;
				break;
			}
		}
		if (found) {
			extensions[extension_count++] = desired_extensions[i];
		} else {
			skr_logf(skr_log_warning, "Extension '%s' not available, skipping", desired_extensions[i]);
		}
	}
	free(available_exts);

	// Build list of desired layers
	const char* desired_layers[8];
	uint32_t    desired_layer_count = 0;
	if (_skr_vk.validation_enabled) {
		desired_layers[desired_layer_count++] = "VK_LAYER_KHRONOS_validation";
	}

	// Get available layers
	uint32_t available_layer_count = 0;
	vkEnumerateInstanceLayerProperties(&available_layer_count, NULL);
	VkLayerProperties* available_layers = malloc(available_layer_count * sizeof(VkLayerProperties));
	vkEnumerateInstanceLayerProperties(&available_layer_count, available_layers);

	// Filter layers to only those available
	const char* layers[8];
	uint32_t    layer_count = 0;
	for (uint32_t i = 0; i < desired_layer_count; i++) {
		bool found = false;
		for (uint32_t j = 0; j < available_layer_count; j++) {
			if (strcmp(desired_layers[i], available_layers[j].layerName) == 0) {
				found = true;
				break;
			}
		}
		if (found) {
			layers[layer_count++] = desired_layers[i];
		} else {
			skr_logf(skr_log_warning, "Layer '%s' not available, skipping", desired_layers[i]);
			if (strcmp(desired_layers[i], "VK_LAYER_KHRONOS_validation") == 0) {
				_skr_vk.validation_enabled = false;
			}
		}
	}
	free(available_layers);

	VkInstanceCreateInfo instance_info = {
		.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo        = &app_info,
		.enabledExtensionCount   = extension_count,
		.ppEnabledExtensionNames = extensions,
		.enabledLayerCount       = layer_count,
		.ppEnabledLayerNames     = layers,
	};

	VkResult result = vkCreateInstance(&instance_info, NULL, &_skr_vk.instance);
	if (result != VK_SUCCESS) {
		skr_logf(skr_log_critical, "Failed to create Vulkan instance: 0x%X", result);
		skr_logf(skr_log_info,     "  Enabled extensions (%u):", extension_count);
		for (uint32_t i = 0; i < extension_count; i++)
			skr_logf(skr_log_info, "    - %s", extensions[i]);
		if (layer_count > 0) {
			skr_logf(skr_log_info, "  Enabled layers (%u):", layer_count);
			for (uint32_t i = 0; i < layer_count; i++) {
				skr_logf(skr_log_info, "    - %s", layers[i]);
			}
		}
		skr_log(skr_log_info, "  Tip: If using RenderDoc, ensure it's launched with Vulkan support enabled");
		return false;
	}

	volkLoadInstance(_skr_vk.instance);

	if (_skr_vk.validation_enabled) {
		if (!_skr_vk_create_debug_messenger()) {
			skr_log(skr_log_warning, "Failed to create debug messenger");
		}
	}

	// Pick physical device
	uint32_t device_count = 0;
	vkEnumeratePhysicalDevices(_skr_vk.instance, &device_count, NULL);
	if (device_count == 0) {
		skr_log(skr_log_critical, "No Vulkan-compatible GPUs found");
		return false;
	}

	VkPhysicalDevice devices[32];
	vkEnumeratePhysicalDevices(_skr_vk.instance, &device_count, devices);

	// Prefer discrete GPU over integrated GPU
	_skr_vk.physical_device = devices[0];
	for (uint32_t i = 0; i < device_count; i++) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(devices[i], &props);
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			_skr_vk.physical_device = devices[i];
			skr_logf(skr_log_info, "Selected discrete GPU: %s", props.deviceName);
			break;
		}
	}

	// Get device properties for timing and logging
	VkPhysicalDeviceProperties device_props;
	vkGetPhysicalDeviceProperties(_skr_vk.physical_device, &device_props);

	// Print selected device if we didn't find discrete GPU
	if (_skr_vk.physical_device == devices[0]) {
		skr_logf(skr_log_info, "Using GPU: %s", device_props.deviceName);
	}

	// Store timestamp period for GPU timing
	_skr_vk.timestamp_period = device_props.limits.timestampPeriod;

	// Find queue families
	uint32_t queue_family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(_skr_vk.physical_device, &queue_family_count, NULL);

	VkQueueFamilyProperties queue_families[32];
	vkGetPhysicalDeviceQueueFamilyProperties(_skr_vk.physical_device, &queue_family_count, queue_families);

	// Find graphics queue family
	_skr_vk.graphics_queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < queue_family_count; i++) {
		if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			_skr_vk.graphics_queue_family = i;
			_skr_vk.present_queue_family  = i; // Assume same for now
			break;
		}
	}

	if (_skr_vk.graphics_queue_family == UINT32_MAX) {
		skr_log(skr_log_critical, "Failed to find graphics queue family");
		return false;
	}

	// Find dedicated transfer queue (TRANSFER but not GRAPHICS)
	_skr_vk.transfer_queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < queue_family_count; i++) {
		if ((queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
			!(queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
			_skr_vk.transfer_queue_family = i;
			_skr_vk.has_dedicated_transfer = true;
			break;
		}
	}

	// Fall back to graphics queue for transfers
	if (_skr_vk.transfer_queue_family == UINT32_MAX) {
		_skr_vk.transfer_queue_family = _skr_vk.graphics_queue_family;
		_skr_vk.has_dedicated_transfer = false;
	}

	// Create queue create infos
	float queue_priority = 1.0f;
	VkDeviceQueueCreateInfo queue_infos[2];
	uint32_t queue_info_count = 0;

	// Always create graphics queue
	queue_infos[queue_info_count++] = (VkDeviceQueueCreateInfo){
		.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = _skr_vk.graphics_queue_family,
		.queueCount       = 1,
		.pQueuePriorities = &queue_priority,
	};

	// Create dedicated transfer queue if available
	if (_skr_vk.has_dedicated_transfer) {
		queue_infos[queue_info_count++] = (VkDeviceQueueCreateInfo){
			.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = _skr_vk.transfer_queue_family,
			.queueCount       = 1,
			.pQueuePriorities = &queue_priority,
		};
	}

	const char* device_extensions[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
		VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME,
	};

	// Query available device features
	VkPhysicalDeviceFeatures available_features;
	vkGetPhysicalDeviceFeatures(_skr_vk.physical_device, &available_features);

	// Enable features we need (only if available)
	VkPhysicalDeviceFeatures device_features = {
		.samplerAnisotropy = available_features.samplerAnisotropy,
		.sampleRateShading = VK_FALSE, // Not using sample shading yet
		.fillModeNonSolid  = VK_FALSE, // Not using wireframe
	};

	VkDeviceCreateInfo device_info = {
		.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext                   = NULL,
		.queueCreateInfoCount    = queue_info_count,
		.pQueueCreateInfos       = queue_infos,
		.enabledExtensionCount   = sizeof(device_extensions) / sizeof(device_extensions[0]),
		.ppEnabledExtensionNames = device_extensions,
		.pEnabledFeatures        = &device_features,
	};

	vr = vkCreateDevice(_skr_vk.physical_device, &device_info, NULL, &_skr_vk.device);
	SKR_VK_CHECK_RET(vr, "vkCreateDevice", false);

	volkLoadDevice(_skr_vk.device);

	// Get graphics queue
	vkGetDeviceQueue(_skr_vk.device, _skr_vk.graphics_queue_family, 0, &_skr_vk.graphics_queue);
	_skr_vk.present_queue = _skr_vk.graphics_queue;

	// Get transfer queue (same as graphics if no dedicated queue)
	if (_skr_vk.has_dedicated_transfer) {
		vkGetDeviceQueue(_skr_vk.device, _skr_vk.transfer_queue_family, 0, &_skr_vk.transfer_queue);
	} else {
		_skr_vk.transfer_queue = _skr_vk.graphics_queue;
	}

	// Create command pool
	VkCommandPoolCreateInfo pool_info = {
		.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = _skr_vk.graphics_queue_family,
	};

	vr = vkCreateCommandPool(_skr_vk.device, &pool_info, NULL, &_skr_vk.command_pool);
	SKR_VK_CHECK_RET(vr, "vkCreateCommandPool", false);
	_skr_cmd_destroy_command_pool(&_skr_vk.destroy_list, _skr_vk.command_pool);

	// Allocate command buffers (one per frame in flight)
	VkCommandBufferAllocateInfo alloc_info = {
		.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool        = _skr_vk.command_pool,
		.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = SKR_MAX_FRAMES_IN_FLIGHT,
	};

	vr = vkAllocateCommandBuffers(_skr_vk.device, &alloc_info, _skr_vk.command_buffers);
	SKR_VK_CHECK_RET(vr, "vkAllocateCommandBuffers", false);

	for (uint32_t i = 0; i < SKR_MAX_FRAMES_IN_FLIGHT; i++) {
		vr = vkCreateFence(_skr_vk.device, &(VkFenceCreateInfo){
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = VK_FENCE_CREATE_SIGNALED_BIT, // Start signaled so first frame doesn't wait
		}, NULL, &_skr_vk.frame_fences[i]);
		SKR_VK_CHECK_RET(vr, "vkCreateFence", false);
		_skr_cmd_destroy_fence(&_skr_vk.destroy_list, _skr_vk.frame_fences[i]);
	}

	vr = vkCreateQueryPool(_skr_vk.device, &(VkQueryPoolCreateInfo){
		.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType  = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = 2 * SKR_MAX_FRAMES_IN_FLIGHT,
	}, NULL, &_skr_vk.timestamp_pool);
	SKR_VK_CHECK_RET(vr, "vkCreateQueryPool", false);
	_skr_cmd_destroy_query_pool(&_skr_vk.destroy_list, _skr_vk.timestamp_pool);

	for (uint32_t i = 0; i < SKR_MAX_FRAMES_IN_FLIGHT; i++) {
		_skr_vk.timestamps_valid[i] = false;
	}

	vr = vkCreatePipelineCache(_skr_vk.device, &(VkPipelineCacheCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
	}, NULL, &_skr_vk.pipeline_cache);
	SKR_VK_CHECK_RET(vr, "vkCreatePipelineCache", false);
	_skr_cmd_destroy_pipeline_cache(&_skr_vk.destroy_list, _skr_vk.pipeline_cache);

	// Create descriptor pool for compute shaders
	VkDescriptorPoolSize pool_sizes[] = {
		{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         .descriptorCount = 1000 },
		{ .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 1000 },
		{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1000 },
		{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         .descriptorCount = 1000 },
	};

	VkDescriptorPoolCreateInfo desc_pool_info = {
		.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		.maxSets       = 1000,
		.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
		.pPoolSizes    = pool_sizes,
	};

	vr = vkCreateDescriptorPool(_skr_vk.device, &desc_pool_info, NULL, &_skr_vk.descriptor_pool);
	SKR_VK_CHECK_RET(vr, "vkCreateDescriptorPool", false);
	_skr_cmd_destroy_descriptor_pool(&_skr_vk.destroy_list, _skr_vk.descriptor_pool);

	_skr_pipeline_init();

	if (!_skr_cmd_init()) {
		skr_log(skr_log_critical, "Failed to initialize upload system");
		return false;
	}

	// Initialize main thread
	skr_thread_init();

	const skr_tex_sampler_t sampler = {
		.sample  = skr_tex_sample_linear,
		.address = skr_tex_address_clamp
	};
	uint32_t color = 0xFFFFFFFF;
	skr_tex_create( skr_tex_fmt_rgba32_linear, skr_tex_flags_readable, sampler, (skr_vec3i_t){1, 1, 1}, 1, 1, &color, &_skr_vk.default_tex_white);
	color = 0xFF808080;
	skr_tex_create( skr_tex_fmt_rgba32_linear, skr_tex_flags_readable, sampler, (skr_vec3i_t){1, 1, 1}, 1, 1, &color, &_skr_vk.default_tex_gray);
	color = 0xFF000000;
	skr_tex_create( skr_tex_fmt_rgba32_linear, skr_tex_flags_readable, sampler, (skr_vec3i_t){1, 1, 1}, 1, 1, &color, &_skr_vk.default_tex_black);

	_skr_vk.initialized = true;
	return true;
}

void skr_shutdown() {
	if (!_skr_vk.initialized) return;

	vkDeviceWaitIdle(_skr_vk.device);

	skr_tex_destroy(&_skr_vk.default_tex_white);
	skr_tex_destroy(&_skr_vk.default_tex_gray);
	skr_tex_destroy(&_skr_vk.default_tex_black);

	_skr_cmd_shutdown     ();
	_skr_pipeline_shutdown();

	_skr_destroy_list_execute(&_skr_vk.destroy_list);
	_skr_destroy_list_free   (&_skr_vk.destroy_list);

	// Destroy device and instance directly (special cases not in destroy list)
	if (_skr_vk.device   != VK_NULL_HANDLE) { vkDestroyDevice  (_skr_vk.device,   NULL); }
	if (_skr_vk.instance != VK_NULL_HANDLE) { vkDestroyInstance(_skr_vk.instance, NULL); }

	memset(&_skr_vk, 0, sizeof(_skr_vk));
}

VkInstance skr_get_vk_instance() {
	return _skr_vk.instance;
}

VkDevice skr_get_vk_device() {
	return _skr_vk.device;
}
