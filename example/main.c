// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "app.h"
#include "tools/scene_util.h"
#include "imgui_backend/imgui_impl_sk_renderer.h"

#include <stdlib.h>
#include <string.h>
#ifndef __ANDROID__
#include <unistd.h>  // chdir
#endif
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

// C wrappers for ImGui SDL2 backend functions
bool ImGui_ImplSDL2_InitForVulkan_C(SDL_Window* window);
void ImGui_ImplSDL2_Shutdown_C(void);
void ImGui_ImplSDL2_NewFrame_C(void);
bool ImGui_ImplSDL2_ProcessEvent_C(const SDL_Event* event);

// SDL-based file reader for su_initialize (handles Android assets via SDL_RWops)
static bool _sdl_file_read(const char* filename, void** out_data, size_t* out_size, void* user_data) {
	(void)user_data;

	SDL_RWops* rw = SDL_RWFromFile(filename, "rb");
	if (!rw) {
		su_log(su_log_critical, "Failed to open file '%s': %s", filename, SDL_GetError());
		return false;
	}

	Sint64 size = SDL_RWsize(rw);
	if (size < 0) {
		su_log(su_log_critical, "Failed to get size of file '%s': %s", filename, SDL_GetError());
		SDL_RWclose(rw);
		return false;
	}

	*out_size = (size_t)size;
	*out_data = malloc(*out_size);
	if (*out_data == NULL) {
		su_log(su_log_critical, "Failed to allocate %zu bytes for file '%s'", *out_size, filename);
		SDL_RWclose(rw);
		*out_size = 0;
		return false;
	}

	size_t bytes_read = SDL_RWread(rw, *out_data, 1, *out_size);
	SDL_RWclose(rw);

	if (bytes_read != *out_size) {
		su_log(su_log_critical, "Failed to read file '%s': expected %zu bytes, got %zu", filename, *out_size, bytes_read);
		free(*out_data);
		*out_data = NULL;
		*out_size = 0;
		return false;
	}

	return true;
}


int main(int argc, char* argv[]) {
	// Parse command line arguments
	int test_frames   = 0;   // 0 = run normally, >0 = exit after N frames
	int start_scene   = -1;  // -1 = use default, >= 0 = start with this scene
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-test") == 0) {
			test_frames = 10;  // Default test mode: 10 frames
		} else if (strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
			test_frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-scene") == 0 && i + 1 < argc) {
			start_scene = atoi(argv[++i]);
		}
	}

	// Set working directory to executable's path for asset loading
#ifndef __ANDROID__
	char* base_path = SDL_GetBasePath();
	if (base_path) {
		chdir(base_path);
		SDL_free(base_path);
	}
#endif

	// Configuration
	const bool enable_validation = true;
	const bool enable_offscreen  = false;
	const bool enable_bloom      = true && enable_offscreen;

	// Initialize SDL
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		su_log(su_log_critical, "SDL initialization failed: %s", SDL_GetError());
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
		2560, 1440,
		SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
#endif
	if (!window) {
		su_log(su_log_critical, "Failed to create SDL window: %s", SDL_GetError());
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
		su_log(su_log_critical, "Failed to initialize sk_renderer!");
		free(extensions);
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}
	free(extensions);

	// Create Vulkan surface
	VkSurfaceKHR vk_surface;
	if (!SDL_Vulkan_CreateSurface(window, skr_get_vk_instance(), &vk_surface)) {
		su_log(su_log_critical, "Failed to create Vulkan surface: %s", SDL_GetError());
		skr_shutdown();
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	// Create sk_renderer surface
	skr_surface_t surface;
	skr_surface_create(vk_surface, &surface);
	if (surface.surface == VK_NULL_HANDLE) {
		su_log(su_log_critical, "Failed to create surface!");
		skr_shutdown();
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	su_log(su_log_info, "sk_renderer initialized successfully!");

	// Initialize scene utilities with SDL file reader (handles Android assets)
	su_initialize(_sdl_file_read, NULL);

	// Initialize ImGui
	igCreateContext(NULL);
	ImGuiIO* io = igGetIO();
	io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls

	// Build font atlas at larger size for crisp rendering
	ImFontConfig* font_cfg = ImFontConfig_ImFontConfig();
	font_cfg->SizePixels = 16.0f;  // Larger default font (13px default -> 20px)
	ImFontAtlas_AddFontDefault(io->Fonts, font_cfg);

	// Initialize ImGui SDL2 backend
	ImGui_ImplSDL2_InitForVulkan_C(window);

	// Initialize ImGui sk_renderer backend
	if (!ImGui_ImplSkRenderer_Init()) {
		su_log(su_log_critical, "Failed to initialize ImGui sk_renderer backend!");
		skr_surface_destroy(&surface);
		skr_shutdown();
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	su_log(su_log_info, "ImGui initialized successfully!");

	// Create application
	app_t* app = app_create(start_scene);
	if (!app) {
		su_log(su_log_critical, "Failed to create application!");
		ImGui_ImplSkRenderer_Shutdown();
		ImGui_ImplSDL2_Shutdown_C();
		igDestroyContext(NULL);
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
		frame_count++;

		// Exit after N frames in test mode
		if (test_frames > 0 && frame_count >= test_frames) {
			running = false;
			break;
		}

		// Handle events
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			// Pass event to ImGui first
			ImGui_ImplSDL2_ProcessEvent_C(&event);

			if (event.type == SDL_QUIT) {
				running = false;
			} else if (event.type == SDL_WINDOWEVENT) {
				if (event.window.event == SDL_WINDOWEVENT_MINIMIZED) {
					suspended = true;
				} else if (event.window.event == SDL_WINDOWEVENT_RESTORED) {
					suspended = false;
				}
			} else if (event.type == SDL_APP_WILLENTERBACKGROUND) {
				su_log(su_log_info, "App entering background - suspending rendering");
				suspended = true;
			} else if (event.type == SDL_APP_DIDENTERFOREGROUND) {
				su_log(su_log_info, "App entering foreground - resuming rendering");
				suspended = false;
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

		// Start ImGui frame
		ImGui_ImplSkRenderer_NewFrame();
		ImGui_ImplSDL2_NewFrame_C();
		igNewFrame();

		skr_renderer_frame_begin();

		app_update      (app, delta_time);
		app_render_imgui(app, NULL, surface.size.x, surface.size.y);

		// Finalize ImGui rendering to get draw data
		igRender();

		// Get next swapchain image
		skr_tex_t*   target         = NULL;
		skr_acquire_ acquire_result = skr_surface_next_tex(&surface, &target);

		if (acquire_result == skr_acquire_success && target) {
			// Render (ImGui is rendered inside app_render, in the same pass)
			app_render(app, target, surface.size.x, surface.size.y);

			// End frame with surface synchronization
			skr_surface_t* surfaces[] = {&surface};
			skr_renderer_frame_end(surfaces, 1);

			// Present
			skr_surface_present(&surface);

		} else { // Failed to acquire swapchain image!
			skr_renderer_frame_end(NULL, 0);
			if (!running) {
				su_log(su_log_info, "Surface issue during shutdown - exiting gracefully");
				break;
			}

			if (acquire_result == skr_acquire_needs_resize) {
				skr_surface_resize(&surface);
			} else if (acquire_result == skr_acquire_surface_lost) {
				// Surface was lost (Android app resume) - need to recreate from SDL
				su_log(su_log_info, "Recreating surface after loss");
				vkDeviceWaitIdle(skr_get_vk_device());

				VkSurfaceKHR new_vk_surface;
				if (!SDL_Vulkan_CreateSurface(window, skr_get_vk_instance(), &new_vk_surface)) {
					su_log(su_log_critical, "Failed to recreate SDL Vulkan surface: %s", SDL_GetError());
					running = false;
					break;
				}

				skr_surface_destroy(&surface);
				skr_surface_create(new_vk_surface, &surface);
				if (surface.surface == VK_NULL_HANDLE) {
					su_log(su_log_critical, "Failed to recreate sk_renderer surface");
					running = false;
					break;
				}
			}
		}
	}

	su_log(su_log_info, "Completed %d frames, shutting down...", frame_count);

	// Wait for GPU
	vkDeviceWaitIdle(skr_get_vk_device());

	// Cleanup ImGui
	ImGui_ImplSkRenderer_Shutdown();
	ImGui_ImplSDL2_Shutdown_C();
	igDestroyContext(NULL);

	// Cleanup
	app_destroy(app);
	skr_surface_destroy(&surface);
	skr_shutdown();
	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}