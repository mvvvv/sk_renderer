cmake_minimum_required(VERSION 3.14)

# OpenXR Example for sk_renderer
# Demonstrates VR rendering using OpenXR with Vulkan backend

# Ensure our cmake/ folder is in path to override FindMetalTools (skip Metal on macOS)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(FetchContent)

###################################
# OpenXR Loader via FetchContent
###################################

message(STATUS "VULKAN_HEADERS_INSTALL_DIR = ${VULKAN_HEADERS_INSTALL_DIR}")
FetchContent_Declare(
	openxr_loader
	GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
	GIT_TAG        release-1.1.43
	GIT_SHALLOW    TRUE
)

# Only build the loader, not samples/tests
set(BUILD_ALL_EXTENSIONS ON CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(openxr_loader)

###################################
# Executable
###################################

add_skapp(sk_renderer_xr_test
	PACKAGE_NAME net.stereokit.renderer_test_xr
	APP_NAME "sk_renderer_test_xr"
	MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/AndroidManifest.xml)

target_sources(sk_renderer_xr_test PRIVATE
	main.c
	app_xr.c
)

target_link_libraries(sk_renderer_xr_test PRIVATE
	sk_renderer
	openxr_loader
)

target_include_directories(sk_renderer_xr_test PRIVATE
	${openxr_loader_SOURCE_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/../example/tools  # For float_math.h
	${CMAKE_CURRENT_SOURCE_DIR}/../sk_renderer    # For vk/skr_vulkan.h
)

# Vulkan headers are already available via FetchContent in the root CMakeLists.txt
# (vulkan_headers target and Vulkan_INCLUDE_DIR are set there)
target_include_directories(sk_renderer_xr_test PRIVATE ${Vulkan_INCLUDE_DIR})

# Math library on Unix
if(UNIX AND NOT APPLE)
	target_link_libraries(sk_renderer_xr_test PRIVATE m)
endif()

###################################
# Shader compilation
###################################

set(ASSET_PATH ${CMAKE_CURRENT_BINARY_DIR}/assets)

# Use skshaderc to compile test.hlsl from the example directory
# The 4th parameter (_) is a placeholder for OUT_LIST so shader goes to ARGN
skshaderc_compile_assets(sk_renderer_xr_test ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders "" _
	../example/assets/shaders/test.hlsl
)
target_skapp_assets(sk_renderer_xr_test ${CMAKE_CURRENT_BINARY_DIR}/generated)

###################################
# Run target
###################################

add_custom_target(run_xr DEPENDS sk_renderer_xr_test
	COMMAND $<TARGET_FILE:sk_renderer_xr_test>
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

