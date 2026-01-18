cmake_minimum_required(VERSION 3.10)
project(sk_renderer VERSION 0.1.0)

# Add custom cmake modules (includes headers-only FindVulkan for cross-compilation)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Build options
option(SKR_BUILD_SKSHADERC "Build skshaderc shader compiler" ON)
option(SKR_BUILD_EXAMPLES "Build sk_renderer example application" ON)
option(SKR_BUILD_XR_EXAMPLE "Build OpenXR example application (Linux/Windows only)" OFF)

include(FetchContent)

###############################################################################
# skshaderc shader compiler
###############################################################################

if(SKR_BUILD_SKSHADERC)
	if(ANDROID)
		# For Android builds, use pre-built skshaderc from bin/
		# (Build the host binary first with a native build)
		add_subdirectory(${CMAKE_SOURCE_DIR}/bin bin_tools)
	else()
		# Native builds: build skshaderc from source
		add_subdirectory(skshaderc)
	endif()
endif()

###############################################################################
# sk_renderer library
###############################################################################

if(ANDROID)
	add_library(sk_renderer STATIC)
	# Enable position-independent code for static library on Android
	set_target_properties(sk_renderer PROPERTIES POSITION_INDEPENDENT_CODE ON)
else()
	add_library(sk_renderer)
endif()

set_target_properties(sk_renderer PROPERTIES
	C_STANDARD 11
	C_STANDARD_REQUIRED ON)

# If we are building skshaderc, then sk_renderer should make skshaderc build
# first. This allows consuming projects to use skshaderc's compile functions.
if(NOT ANDROID AND SKR_BUILD_SKSHADERC)
	add_dependencies(sk_renderer skshaderc)
endif()

# Grab Vulkan headers first before Volk goes looking for its own. Good for a
# few reasons, but notably avoids a problem when cross-compiling with mingw.
message(STATUS "Fetching Vulkan-Headers...")
FetchContent_Declare(
	vulkan_headers
	URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.336.zip
)
FetchContent_MakeAvailable(vulkan_headers)
# Set paths for volk and our custom FindVulkan.cmake
set(VULKAN_HEADERS_INSTALL_DIR "${vulkan_headers_SOURCE_DIR}" CACHE PATH "Vulkan headers for volk" FORCE)
set(Vulkan_INCLUDE_DIR "${vulkan_headers_SOURCE_DIR}/include" CACHE PATH "Vulkan headers for FindVulkan" FORCE)

message(STATUS "Fetching volk...")
FetchContent_Declare( volk
	GIT_REPOSITORY https://github.com/zeux/volk.git
	GIT_TAG        1.3.270
	GIT_SHALLOW    TRUE
	GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(volk)
target_link_libraries(sk_renderer PUBLIC volk)
if(ANDROID)
	set_target_properties(volk PROPERTIES POSITION_INDEPENDENT_CODE ON)
	target_link_libraries(sk_renderer PRIVATE log)  # Android logging library
endif()

# Set symbol visibility to hidden by default, only export what's marked with SKR_API/SKSC_API
if(NOT MSVC)
	target_compile_options    (sk_renderer PRIVATE -fvisibility=hidden)
endif()
if (MINGW)
	# mingw does not support C11 threads, we have a small fallback
	target_link_libraries     (sk_renderer PRIVATE pthread)
	target_include_directories(sk_renderer PRIVATE sk_renderer/threads)
endif()

target_include_directories(sk_renderer PUBLIC sk_renderer/include)
target_sources(sk_renderer PRIVATE
	sk_renderer/include/sk_renderer.h
	sk_renderer/skr_log.c
	sk_renderer/sksc_file.c
	sk_renderer/vk/_sk_renderer.h
	sk_renderer/vk/skr_initialize.c
	sk_renderer/vk/skr_renderer.c
	sk_renderer/vk/skr_surface.c
	sk_renderer/vk/skr_buffer.c
	sk_renderer/vk/skr_shader.c
	sk_renderer/vk/skr_material.c
	sk_renderer/vk/skr_mesh.c
	sk_renderer/vk/skr_render_list.c
	sk_renderer/vk/skr_texture.c
	sk_renderer/vk/skr_pipeline.h
	sk_renderer/vk/skr_pipeline.c
	sk_renderer/vk/skr_compute.c
	sk_renderer/vk/skr_command.c
	sk_renderer/vk/skr_conversions.c
	sk_renderer/vk/skr_debug.c
	sk_renderer/vk/skr_destroy_list.c
)

###############################################################################
# Example application
###############################################################################

if(SKR_BUILD_EXAMPLES)
	if(NOT SKR_BUILD_SKSHADERC)
		message(WARNING "SKR_BUILD_EXAMPLES requires SKR_BUILD_SKSHADERC to be enabled. Disabling examples.")
		set(SKR_BUILD_EXAMPLES OFF)
	else()
		# Set Android NDK path for example project
		if(ANDROID)
			set(ANDROID_NDK ${CMAKE_ANDROID_NDK})
		endif()

		add_subdirectory(example)
		add_subdirectory(example_xr)
	endif()
endif() # SKR_BUILD_EXAMPLES