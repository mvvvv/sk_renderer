cmake_minimum_required(VERSION 3.10)
project(sk_renderer_example VERSION 0.1.0 LANGUAGES CXX C)

include(FetchContent)

###############################################################################
# Dependencies
###############################################################################

# SDL2 for windowing and platform abstraction
set(SDL_PIPEWIRE OFF CACHE BOOL "Disable PipeWire support" FORCE)
FetchContent_Declare( sdl2
	GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
	GIT_TAG        release-2.30.10
	GIT_SHALLOW    TRUE
	GIT_PROGRESS   TRUE
	OPTIONS
		"SDL_TEST_LIBRARY OFF"
		"SDL_SHARED ON"
		"SDL_STATIC OFF"
)
FetchContent_MakeAvailable(sdl2)

# stb for image loading
FetchContent_Declare( stb
	GIT_REPOSITORY https://github.com/nothings/stb.git
	GIT_TAG        master
	GIT_SHALLOW    TRUE
	GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(stb)

# cgltf for GLTF model loading
FetchContent_Declare( cgltf
	GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
	GIT_TAG        v1.14
	GIT_SHALLOW    TRUE
	GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(cgltf)

# cimgui C bindings for ImGui
FetchContent_Declare( cimgui
	GIT_REPOSITORY https://github.com/cimgui/cimgui.git
	GIT_TAG        1.91.6
	GIT_SHALLOW    TRUE
	GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(cimgui)

# FFmpeg for video playback (Linux only for now)
if(UNIX AND NOT ANDROID AND NOT APPLE)
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		pkg_check_modules(FFMPEG
			libavcodec>=58.0
			libavformat>=58.0
			libavutil>=56.0
			libswscale>=5.0
		)
		if(FFMPEG_FOUND)
			message(STATUS "FFmpeg found - video playback enabled")
			set(SKR_HAS_VIDEO TRUE)
		else()
			message(STATUS "FFmpeg not found - video playback disabled")
			set(SKR_HAS_VIDEO FALSE)
		endif()
	else()
		message(STATUS "PkgConfig not found - video playback disabled")
		set(SKR_HAS_VIDEO FALSE)
	endif()
else()
	set(SKR_HAS_VIDEO FALSE)
endif()

###############################################################################
# sk_renderer_test target
###############################################################################

if(ANDROID)
	add_library(sk_renderer_test SHARED)
else()
	add_executable(sk_renderer_test)
endif()

if (ANDROID)
	set_target_properties(sk_renderer_test PROPERTIES OUTPUT_NAME "main")
elseif (UNIX)
	target_link_libraries(sk_renderer_test PRIVATE m)
elseif(WIN32)
	target_link_libraries(sk_renderer_test PRIVATE SDL2::SDL2main)
endif()

# Static link MinGW runtime and pthread when cross-compiling to Windows
if(MINGW)
	target_link_options       (sk_renderer_test PRIVATE -static-libgcc -static-libstdc++ -static)
	target_link_libraries     (sk_renderer_test PRIVATE pthread)
	target_include_directories(sk_renderer_test PRIVATE ${CMAKE_SOURCE_DIR}/sk_renderer/threads)
endif()

target_link_libraries     (sk_renderer_test PRIVATE SDL2::SDL2 sk_renderer)
target_include_directories(sk_renderer_test PRIVATE ${stb_SOURCE_DIR} ${cgltf_SOURCE_DIR} ${cimgui_SOURCE_DIR}/imgui ${cimgui_SOURCE_DIR}/backends ${cimgui_SOURCE_DIR})
target_sources            (sk_renderer_test PRIVATE
	main.c
	app.c
	scene_util.c
	scene_meshes.c
	scene_reaction_diffusion.c
	scene_orbital_particles.c
	scene_impostor.c
	scene_array_texture.c
	scene_3d_texture.c
	scene_cubemap.c
	scene_gltf.c
	scene_shadows.c
	scene_cloth.c
	bloom.c
	imgui_backend/imgui_impl_sk_renderer.cpp
	imgui_backend/imgui_sdl2_wrapper.cpp
	${cimgui_SOURCE_DIR}/imgui/imgui.cpp
	${cimgui_SOURCE_DIR}/imgui/imgui_draw.cpp
	${cimgui_SOURCE_DIR}/imgui/imgui_tables.cpp
	${cimgui_SOURCE_DIR}/imgui/imgui_widgets.cpp
	${cimgui_SOURCE_DIR}/imgui/imgui_demo.cpp
	${cimgui_SOURCE_DIR}/imgui/backends/imgui_impl_sdl2.cpp
	${cimgui_SOURCE_DIR}/cimgui.cpp
)

# Video playback (conditional on FFmpeg)
if(SKR_HAS_VIDEO)
	target_compile_definitions(sk_renderer_test PRIVATE SKR_HAS_VIDEO)
	target_include_directories(sk_renderer_test PRIVATE ${FFMPEG_INCLUDE_DIRS})
	target_link_libraries     (sk_renderer_test PRIVATE ${FFMPEG_LIBRARIES})
	target_sources            (sk_renderer_test PRIVATE video.c scene_video.c)
endif()

###############################################################################
# Shader compilation
###############################################################################

if(ANDROID)
	set(ASSET_PATH ${CMAKE_CURRENT_BINARY_DIR}/assets)
else()
	set(ASSET_PATH ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Compile ImGui shader to header file (no debug info, embedded in binary)
# Output goes to imgui_backend/ to keep all ImGui backend files together
skshaderc_compile_headers(sk_renderer_test ${CMAKE_CURRENT_SOURCE_DIR}/imgui_backend "-e -t s"
	imgui_backend/imgui.hlsl
)

# Compile other shaders to .sks files
skshaderc_compile_assets(sk_renderer_test ${ASSET_PATH}/shaders ""
	assets/shaders/bloom_composite.hlsl
	assets/shaders/bloom_downsample.hlsl
	assets/shaders/bloom_upsample.hlsl
	assets/shaders/compute_test.hlsl
	assets/shaders/cubemap_mipgen.hlsl
	assets/shaders/cubemap_reflection.hlsl
	assets/shaders/cubemap_skybox.hlsl
	assets/shaders/equirect_to_cubemap.hlsl
	assets/shaders/mipgen_alpha_weighted_render.hlsl
	assets/shaders/orbital_particles_compute.hlsl
	assets/shaders/orbital_particles.hlsl
	assets/shaders/pbr.hlsl
	assets/shaders/shadow_caster.hlsl
	assets/shaders/shadow_receiver.hlsl
	assets/shaders/stereo_display.hlsl
	assets/shaders/test.hlsl
	assets/shaders/texture3d.hlsl
	assets/shaders/video.hlsl
)

###############################################################################
# Android build configuration
###############################################################################

if(ANDROID)
	set(PROJECT_NAME               "sk_renderer_test")  # Tell AndroidBuild which target to package
	set(ANDROID_PACKAGE_NAME       "net.stereokit.renderer_test")
	set(ANDROID_MIN_SDK_VERSION    29)
	set(ANDROID_TARGET_SDK_VERSION 36)
	set(ANDROID_BUILD_TOOLS_VERSION "36.1.0")
	include(${CMAKE_SOURCE_DIR}/example/android/AndroidBuild.cmake)
endif()

###############################################################################
# "run" command
###############################################################################

if(ANDROID)
	# Android run target is defined in AndroidBuild.cmake
else()
	add_custom_target(run DEPENDS sk_renderer_test COMMAND $<TARGET_FILE:sk_renderer_test>)
endif()

###############################################################################
# Asset copying
###############################################################################

# Copy Assets to the folder with the executable
add_custom_command(TARGET sk_renderer_test POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets ${ASSET_PATH}
	COMMENT "Copying App Assets")
