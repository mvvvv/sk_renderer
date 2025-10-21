#include "app.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

// Platform-specific file reading implementation
bool app_read_file(const char* filename, void** out_data, size_t* out_size) {
#ifdef __ANDROID__
	// Use SDL's RWops to read from Android assets
	SDL_RWops* rw = SDL_RWFromFile(filename, "rb");
	if (!rw) return false;

	Sint64 size = SDL_RWsize(rw);
	if (size < 0) {
		SDL_RWclose(rw);
		return false;
	}

	*out_size = (size_t)size;
	*out_data = malloc(*out_size);
	if (*out_data == NULL) {
		SDL_RWclose(rw);
		*out_size = 0;
		return false;
	}

	size_t bytes_read = SDL_RWread(rw, *out_data, 1, *out_size);
	SDL_RWclose(rw);

	return bytes_read == *out_size;
#else
	// Try to open the file directly first
	FILE* fp = fopen(filename, "rb");

	// If that fails, try relative to executable directory
	if (fp == NULL) {
		char exe_path[1024];
		ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
		if (len == -1) return false;
		exe_path[len] = '\0';

		char* last_slash = strrchr(exe_path, '/');
		if (last_slash) *last_slash = '\0';

		// Build full path
		char full_path[2048];
		snprintf(full_path, sizeof(full_path), "%s/%s", exe_path, filename);

		fp = fopen(full_path, "rb");
		if (fp == NULL) return false;
	}

	fseek(fp, 0L, SEEK_END);
	*out_size = ftell(fp);
	rewind(fp);

	*out_data = malloc(*out_size);
	if (*out_data == NULL) {
		*out_size = 0;
		fclose(fp);
		return false;
	}
	fread(*out_data, *out_size, 1, fp);
	fclose(fp);

	return true;
#endif
}


int main(int argc, char* argv[]) {
	char cwd[1024];
	if (getcwd(cwd, sizeof(cwd)) != NULL) {
		skr_logf(skr_log_info, "Working directory: %s", cwd);
	}

	// Configuration
	const bool enable_validation = true;
	const bool enable_offscreen  = false;
	const bool enable_bloom      = true && enable_offscreen;

	// Initialize SDL
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		skr_logf(skr_log_critical, "SDL initialization failed: %s", SDL_GetError());
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
		skr_logf(skr_log_critical, "Failed to create SDL window: %s", SDL_GetError());
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
		skr_logf(skr_log_critical, "Failed to create Vulkan surface: %s", SDL_GetError());
		skr_shutdown();
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	// Create sk_renderer surface
	skr_surface_t surface = skr_surface_create(vk_surface);
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
	bool     resized     = false;
	uint64_t last_time   = SDL_GetPerformanceCounter();

	while (running) {
		// Handle events
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) {
				running = false;
			} else if (event.type == SDL_WINDOWEVENT) {
				if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
					resized = true;
				} else if (event.window.event == SDL_WINDOWEVENT_MINIMIZED) {
					continue;
				}
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

		// Handle resize
		if (resized) {
			vkDeviceWaitIdle(skr_get_vk_device());
			skr_surface_resize(&surface);
			resized = false;
		}

		// Calculate delta time
		uint64_t current_time = SDL_GetPerformanceCounter();
		float    delta_time   = (float)(current_time - last_time) / (float)SDL_GetPerformanceFrequency();
		last_time = current_time;

		// Update application
		app_update(app, delta_time);

		// Get next swapchain image
		skr_tex_t* target = skr_surface_next_tex(&surface);

		// Skip rendering if swapchain is out of date (will be recreated on next frame)
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
			skr_logf(skr_log_info, "GPU frame time: %.4f ms (%.1f FPS)", gpu_ms, 1000.0f / gpu_ms);
		}
	}

	skr_logf(skr_log_info, "Completed %d frames, shutting down...", frame_count);

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