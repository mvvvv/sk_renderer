# The skshaderc cmake file exports a few useful items for
# building shaders in your own project! The first is the
# skshaderc_compile_headers macro, which can compile a list
# of shader files, and return a list with the same files
# and .h appended to them.
#
# Example:
# set(PROJECT_SHADERS_HLSL
#     compute_test.hlsl
#     cubemap.hlsl
#     test.hlsl )
# skshaderc_compile_headers(PROJECT_SHADERS_HLSL "-t xge" PROJECT_SHADERS_H)
#
# PROJECT_SHADERS_H now contains a list of .h files that
# contain shader binaries in arrays.
#
# skshaderc_compile_assets is similar, but is not specifically
# for header files. It's best if you're looking for side-by-side
# .sks files. It has no output list.
#
# skshaderc_compile_assets(PROJECT_SHADERS_HLSL "-t xge")
#
# If you wish to design your own shader compiling function,
# this file also exports SKSHADERC_EXE_PATH, which is a 
# generator for the path to the skshaderc compiler executable
# file.

cmake_minimum_required(VERSION 3.10)
project(skshaderc VERSION 1.0 DESCRIPTION "Shader compiler for sk_renderer")

include(FetchContent)
set(VULKAN_VERSION vulkan-sdk-1.4.328.1) # Most of our dependencies here use the same version tag!

###############################################################################
# Static library: skshaderc_lib
###############################################################################

add_library(skshaderc_lib STATIC)

set_target_properties(skshaderc_lib PROPERTIES
	CXX_STANDARD 11
	CXX_STANDARD_REQUIRED ON)

# For SPIRV-Tools
message(STATUS "Fetching SPIRV-Headers...")
FetchContent_Declare( SPIRV-Headers
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
	GIT_TAG        ${VULKAN_VERSION}
	GIT_SHALLOW    TRUE
	GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(SPIRV-Headers)
target_link_libraries     (skshaderc_lib PUBLIC SPIRV)

# For optimizing SPIR-V shaders, a baseline amount of
# optimization is crucial for meta compatability with
# HLSL compilers.
message(STATUS "Fetching SPIRV-Tools...")
FetchContent_Declare( SPIRV-Tools
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
	GIT_TAG        ${VULKAN_VERSION}
	GIT_SHALLOW    TRUE
	GIT_PROGRESS   TRUE
)
set(SPIRV_SKIP_TESTS       ON  CACHE BOOL "")
set(SPIRV_SKIP_EXECUTABLES ON  CACHE BOOL "")
set(SPIRV_WERROR           OFF CACHE BOOL "")
FetchContent_MakeAvailable(SPIRV-Tools)
target_link_libraries     (skshaderc_lib PUBLIC SPIRV-Tools-opt)

# For grabbing shader metadata, SPIRV-Cross can also do
# this, but Reflect is lighter weight.
message(STATUS "Fetching SPIRV-Reflect...")
FetchContent_Declare( SPIRV-Reflect
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Reflect.git
	GIT_TAG        ${VULKAN_VERSION}
	GIT_SHALLOW    TRUE
	GIT_PROGRESS   TRUE
)
set(SPIRV_REFLECT_EXECUTABLE OFF CACHE BOOL "")
set(SPIRV_REFLECT_STATIC_LIB ON CACHE BOOL "")
FetchContent_MakeAvailable(SPIRV-Reflect)
target_link_libraries     (skshaderc_lib PUBLIC spirv-reflect-static)

# This is used to convert HLSL to SPIR-V. DXC is an
# alternative, but is far more difficult to compile,
# and generates a much larger binary.
message(STATUS "Fetching glslang...")
FetchContent_Declare( glslang
	GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
	GIT_TAG        ${VULKAN_VERSION}
	GIT_SHALLOW    TRUE
	GIT_PROGRESS   TRUE
)
set(GLSLANG_ENABLE_INSTALL  OFF  CACHE BOOL "")
set(GLSLANG_TESTS           OFF  CACHE BOOL "")
set(ENABLE_GLSLANG_BINARIES OFF  CACHE BOOL "")
set(ENABLE_SPVREMAPPER      OFF  CACHE BOOL "")
set(ENABLE_GLSLANG_JS       OFF  CACHE BOOL "")
set(ENABLE_HLSL             ON   CACHE BOOL "")
set(ENABLE_RTTI             OFF  CACHE BOOL "")
FetchContent_MakeAvailable(glslang)
target_link_libraries     (skshaderc_lib PUBLIC glslang)

target_sources(skshaderc_lib PRIVATE
	sksc.cpp
	sksc_hlsl.cpp
	sksc_meta.cpp
	sksc_log.cpp
	sksc.h
	_sksc.h
	miniz.cpp
	miniz.h
	../sk_renderer/sksc_file.c
)
target_include_directories(skshaderc_lib PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	../sk_renderer/include
)

###############################################################################
# Executable: skshaderc
###############################################################################

add_executable(skshaderc main.cpp)
target_link_libraries(skshaderc PRIVATE skshaderc_lib)

# Static link MinGW runtime when cross-compiling to Windows
if(MINGW)
	target_link_options(skshaderc PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

set_target_properties(skshaderc PROPERTIES
	INTERPROCEDURAL_OPTIMIZATION TRUE
)

if (NOT MSVC AND NOT APPLE)
	add_custom_command(TARGET skshaderc POST_BUILD
		COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:skshaderc> $<TARGET_FILE_DIR:skshaderc>/$<TARGET_FILE_BASE_NAME:skshaderc>.dbg
		COMMAND ${CMAKE_STRIP} --strip-debug $<TARGET_FILE:skshaderc>
		COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink=$<TARGET_FILE_DIR:skshaderc>/$<TARGET_FILE_BASE_NAME:skshaderc>.dbg $<TARGET_FILE:skshaderc>
	)
endif()

set(SKSHADERC_EXE_PATH $<TARGET_FILE:skshaderc> CACHE INTERNAL "Path to skshaderc executable")

# Detect cross-compilation to Windows from Unix - use Wine to run skshaderc.exe
if(CMAKE_HOST_UNIX AND CMAKE_SYSTEM_NAME MATCHES "Windows")
	set(WINE "wine" CACHE INTERNAL "")
endif()

###############################################################################
# Install skshaderc binary to bin/tools/{platform}/ for distribution
###############################################################################

# Determine the platform-specific installation directory
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
		set(SKSHADERC_INSTALL_SUBDIR "tools/win32_x64")
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
		set(SKSHADERC_INSTALL_SUBDIR "tools/win32_arm64")
	endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	set(SKSHADERC_INSTALL_SUBDIR "tools/mac")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
		set(SKSHADERC_INSTALL_SUBDIR "tools/linux_x64")
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
		set(SKSHADERC_INSTALL_SUBDIR "tools/linux_arm64")
	endif()
endif()

# Add a custom target to copy the binary to bin/ in the repository root
# Use CMAKE_SOURCE_DIR so binaries go to the top-level project's bin/,
# allowing consuming projects to accumulate skshaderc builds.
if(DEFINED SKSHADERC_INSTALL_SUBDIR)
	set(SKSHADERC_BIN_DIR "${CMAKE_SOURCE_DIR}/bin/${SKSHADERC_INSTALL_SUBDIR}")
	add_custom_command(TARGET skshaderc POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${SKSHADERC_BIN_DIR}"
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:skshaderc> "${SKSHADERC_BIN_DIR}/"
		COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.distribute.txt" "${CMAKE_SOURCE_DIR}/bin/CMakeLists.txt"
		COMMENT "Installing skshaderc to ${SKSHADERC_BIN_DIR}"
	)
	message(STATUS "skshaderc will be copied to: ${SKSHADERC_BIN_DIR}")
else()
	message(WARNING "Unknown platform for skshaderc installation: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# functions for compiling shaders easily
function(SKSHADERC_COMPILE_ASSETS TARGET OUT_PATH COMMAND_STRING OUT_LIST)
	set(SKSHADERC_COMPILE_COMMANDS "-e $<$<CONFIG:Debug>:-d> -o ${OUT_PATH} ${COMMAND_STRING}")
	message(STATUS "sk_renderer compiling shader assets with args 'skshaderc ${SKSHADERC_COMPILE_COMMANDS} [shaders]'")
	separate_arguments(SKSHADERC_COMPILE_COMMANDS)

	set(SHADER_LIST)
	set(COMPILED_SHADERS)

	foreach(SHADER IN LISTS ARGN)
		get_filename_component(SHADER_NAME ${SHADER} NAME)
		set(COMPILED_SHADER "${OUT_PATH}/${SHADER_NAME}.sks")

		add_custom_command(
			OUTPUT ${COMPILED_SHADER}
			COMMAND ${WINE} ${SKSHADERC_EXE_PATH} ${SKSHADERC_COMPILE_COMMANDS} ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}
			DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}
			VERBATIM
		)
		list(APPEND SHADER_LIST ${SHADER})
		list(APPEND COMPILED_SHADERS ${COMPILED_SHADER})
	endforeach(SHADER)

	add_custom_target(${TARGET}_shaders DEPENDS ${COMPILED_SHADERS})
	add_dependencies(${TARGET} ${TARGET}_shaders)

	set(${OUT_LIST} ${SHADER_LIST} PARENT_SCOPE)
endfunction()

function(SKSHADERC_COMPILE_HEADERS ADD_TARGET OUTPUT_FOLDER COMMAND_STRING)
	set(SKSHADERC_COMPILE_COMMANDS "-h $<$<CONFIG:Debug>:-d> -o ${OUTPUT_FOLDER} ${COMMAND_STRING}")
	message(STATUS "sk_renderer compiling shader headers with args 'skshaderc ${SKSHADERC_COMPILE_COMMANDS} [shaders]'")
	separate_arguments(SKSHADERC_COMPILE_COMMANDS)

	set(SHADER_LIST)
	foreach(SHADER IN LISTS ARGN)
		get_filename_component(SHADER_NAME ${SHADER} NAME)
		add_custom_command(
			OUTPUT ${OUTPUT_FOLDER}/${SHADER_NAME}.h
			COMMAND ${WINE} ${SKSHADERC_EXE_PATH} ${SKSHADERC_COMPILE_COMMANDS} ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}
			VERBATIM
		)
		list(APPEND SHADER_LIST ${OUTPUT_FOLDER}/${SHADER_NAME}.h)
	endforeach(SHADER)

	target_include_directories(${ADD_TARGET} PRIVATE ${OUTPUT_FOLDER})
	target_sources            (${ADD_TARGET} PRIVATE ${SHADER_LIST})
endfunction()