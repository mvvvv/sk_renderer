cmake_minimum_required(VERSION 3.14)

# OpenXR Example for sk_renderer
# Demonstrates VR rendering using OpenXR with Vulkan backend

include(FetchContent)

###################################
# OpenXR Loader via FetchContent
###################################

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

FetchContent_MakeAvailable(openxr_loader)

###################################
# Executable
###################################

add_executable(sk_renderer_xr_test
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

# We need Vulkan headers for the OpenXR Vulkan extension
find_package(Vulkan REQUIRED)
target_link_libraries(sk_renderer_xr_test PRIVATE Vulkan::Headers)

# Math library on Unix
if(UNIX AND NOT APPLE)
    target_link_libraries(sk_renderer_xr_test PRIVATE m)
endif()

###################################
# Shader compilation
###################################

set(ASSET_PATH ${CMAKE_CURRENT_BINARY_DIR}/assets)

# Use skshaderc to compile test.hlsl from the example directory
# Note: paths must be relative to CMAKE_CURRENT_SOURCE_DIR
# The 4th parameter (_) is a placeholder for OUT_LIST so shader goes to ARGN
skshaderc_compile_assets(sk_renderer_xr_test ${ASSET_PATH}/shaders "" _
    ../example/assets/shaders/test.hlsl
)

# Create asset directory
add_custom_command(TARGET sk_renderer_xr_test PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ASSET_PATH}/shaders
    COMMENT "Creating asset directories for XR example"
)

###################################
# Run target
###################################

add_custom_target(run_xr DEPENDS sk_renderer_xr_test
    COMMAND $<TARGET_FILE:sk_renderer_xr_test>
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
