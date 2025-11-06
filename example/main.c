// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "app.h"

#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>


int main(int argc, char* argv[]) {

	// Configuration
	const bool enable_validation = true;
	const bool enable_offscreen  = false;
	const bool enable_bloom      = true && enable_offscreen;

	// Initialize SDL
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		skr_log(skr_log_critical, "SDL initialization failed: %s", SDL_GetError());
		return 1;
	}

	// Create SDL window
	SDL_Window* window = NULL;
#ifdef __ANDROID__
	window = SDL_CreateWindow("sk_renderer_test",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		0, 0,
		SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
#else
	window = SDL_CreateWindow("sk_renderer_test",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		1280, 720,
		SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
#endif
	if (!window) {
		skr_log(skr_log_critical, "Failed to create SDL window: %s", SDL_GetError());
		SDL_Quit();
		return 1;
	}

	// Get required Vulkan extensions from SDL
	uint32_t extension_count = 0;
	SDL_Vulkan_GetInstanceExtensions(window, &extension_count, NULL);
	const char** extensions = malloc(extension_count * sizeof(char*));
	SDL_Vulkan_GetInstanceExtensions(window, &extension_count, extensions);

	// Initialize sk_renderer
	skr_settings_t settings = {
		.app_name                 = "sk_renderer_test",
		.app_version              = 1,
		.enable_validation        = enable_validation,
		.required_extensions      = extensions,
		.required_extension_count = extension_count,
	};

	if (!skr_init(settings)) {
		skr_log(skr_log_critical, "Failed to initialize sk_renderer!");
		free(extensions);
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}
	free(extensions);

	// Create Vulkan surface
	VkSurfaceKHR vk_surface;
	if (!SDL_Vulkan_CreateSurface(window, skr_get_vk_instance(), &vk_surface)) {
		skr_log(skr_log_critical, "Failed to create Vulkan surface: %s", SDL_GetError());
		skr_shutdown();
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	// Create sk_renderer surface
	skr_surface_t surface;
	skr_surface_create(vk_surface, &surface);
	if (surface.surface == VK_NULL_HANDLE) {
		skr_log(skr_log_critical, "Failed to create surface!");
		skr_shutdown();
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	skr_log(skr_log_info, "sk_renderer initialized successfully!");

	// Create application
	app_t* app = app_create();
	if (!app) {
		skr_log(skr_log_critical, "Failed to create application!");
		skr_surface_destroy(&surface);
		skr_shutdown();
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	// Main loop
	int      frame_count = 0;
	bool     running     = true;
	bool     suspended   = false;
	uint64_t last_time   = SDL_GetPerformanceCounter();

	while (running) {
		// Handle events
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) {
				running = false;
			} else if (event.type == SDL_WINDOWEVENT) {
				if (event.window.event == SDL_WINDOWEVENT_MINIMIZED) {
					suspended = true;
				} else if (event.window.event == SDL_WINDOWEVENT_RESTORED) {
					suspended = false;
				}
			} else if (event.type == SDL_APP_WILLENTERBACKGROUND) {
				skr_log(skr_log_info, "App entering background - suspending rendering");
				suspended = true;
			} else if (event.type == SDL_APP_DIDENTERFOREGROUND) {
				skr_log(skr_log_info, "App entering foreground - resuming rendering");
				suspended = false;
			} else if (event.type == SDL_KEYDOWN) {
				if (event.key.keysym.sym == SDLK_LEFT) {
					app_key_press(app, app_key_left);
				} else if (event.key.keysym.sym == SDLK_RIGHT) {
					app_key_press(app, app_key_right);
				}
			} else if (event.type == SDL_MOUSEBUTTONDOWN) {
				app_key_press(app, app_key_right);
			}
		}

		// Skip rendering and updates while suspended (backgrounded/minimized)
		if (suspended) {
			SDL_Delay(100);  // Reduce CPU usage while suspended
			continue;
		}

		// Calculate delta time
		uint64_t current_time = SDL_GetPerformanceCounter();
		float    delta_time   = (float)(current_time - last_time) / (float)SDL_GetPerformanceFrequency();
		last_time = current_time;

		// Update application
		app_update(app, delta_time);

		// Get next swapchain image
		skr_tex_t*   target        = NULL;
		skr_acquire_ acquire_result = skr_surface_next_tex(&surface, &target);

		// Handle acquisition errors
		if (acquire_result == skr_acquire_surface_lost) {
			// Surface was lost - only try to recreate if we're not shutting down
			if (!running) {
				skr_log(skr_log_info, "Surface lost during shutdown - exiting gracefully");
				break;
			}

			// Surface was lost (Android app resume) - need to recreate from SDL
			skr_log(skr_log_info, "Recreating surface after loss");
			vkDeviceWaitIdle(skr_get_vk_device());

			VkSurfaceKHR new_vk_surface;
			if (!SDL_Vulkan_CreateSurface(window, skr_get_vk_instance(), &new_vk_surface)) {
				skr_log(skr_log_critical, "Failed to recreate SDL Vulkan surface: %s", SDL_GetError());
				running = false;
				break;
			}

			skr_surface_destroy(&surface);
			skr_surface_create(new_vk_surface, &surface);
			if (surface.surface == VK_NULL_HANDLE) {
				skr_log(skr_log_critical, "Failed to recreate sk_renderer surface");
				running = false;
				break;
			}
			continue;
		} else if (acquire_result == skr_acquire_needs_resize) {
			// Swapchain out of date - resize and retry next frame
			vkDeviceWaitIdle(skr_get_vk_device());
			skr_surface_resize(&surface);
			continue;
		} else if (acquire_result != skr_acquire_success) {
			// Other error - if we're shutting down, exit gracefully
			if (!running) {
				skr_log(skr_log_info, "Acquire failed during shutdown - exiting gracefully");
				break;
			}
			// Skip frame and retry
			continue;
		}

		// Render if we successfully acquired an image
		if (target) {
			// Begin frame
			skr_renderer_frame_begin();

			// Render
			app_render(app, target, surface.size.x, surface.size.y);

			// Present
			skr_surface_present(&surface);

			// End frame
			skr_renderer_frame_end();
		}

		// Print GPU timing every 60 frames
		frame_count++;
		if (frame_count % 60 == 0) {
			float gpu_ms = skr_renderer_get_gpu_time_ms();
			skr_log(skr_log_info, "GPU frame time: %.4f ms (%.1f FPS)", gpu_ms, 1000.0f / gpu_ms);
		}
	}

	skr_log(skr_log_info, "Completed %d frames, shutting down...", frame_count);

	// Wait for GPU
	vkDeviceWaitIdle(skr_get_vk_device());

	// Cleanup
	app_destroy(app);
	skr_surface_destroy(&surface);
	skr_shutdown();
	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}