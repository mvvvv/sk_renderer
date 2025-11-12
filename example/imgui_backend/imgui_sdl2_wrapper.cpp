// SPDX-License-Identifier: MIT
// Thin C wrapper for ImGui SDL2 backend

#include <imgui/backends/imgui_impl_sdl2.h>
#include <SDL2/SDL.h>

extern "C" {

bool ImGui_ImplSDL2_InitForVulkan_C(SDL_Window* window) {
    return ImGui_ImplSDL2_InitForVulkan(window);
}

void ImGui_ImplSDL2_Shutdown_C(void) {
    ImGui_ImplSDL2_Shutdown();
}

void ImGui_ImplSDL2_NewFrame_C(void) {
    ImGui_ImplSDL2_NewFrame();
}

bool ImGui_ImplSDL2_ProcessEvent_C(const SDL_Event* event) {
    return ImGui_ImplSDL2_ProcessEvent(event);
}

}
