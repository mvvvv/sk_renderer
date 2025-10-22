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
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})
set(CMAKE_CXX_STANDARD 17)
project(skshaderc VERSION 1.0 DESCRIPTION "Shader compiler for sk_renderer" LANGUAGES CXX)

include(FetchContent)
set(VULKAN_VERSION vulkan-sdk-1.3.296.0) # Most of our dependencies here use the same version tag!

add_executable(skshaderc)

# For converting SPIR-V to flavors of GLSL
#FetchContent_Declare( SPIRV-Cross
#	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
#	GIT_TAG        ${VULKAN_VERSION}
#	GIT_SHALLOW    TRUE
#	GIT_PROGRESS   TRUE
#)
#set(SPIRV_CROSS_CLI            OFF  CACHE BOOL "")
#set(SPIRV_CROSS_ENABLE_TESTS   OFF  CACHE BOOL "")
#set(SPIRV_CROSS_ENABLE_MSL     OFF  CACHE BOOL "")
#set(SPIRV_CROSS_ENABLE_CPP     OFF  CACHE BOOL "")
#set(SPIRV_CROSS_ENABLE_REFLECT OFF  CACHE BOOL "")
#set(SPIRV_CROSS_ENABLE_C_API   OFF  CACHE BOOL "")
#set(SPIRV_CROSS_ENABLE_UTIL    OFF  CACHE BOOL "")
#FetchContent_MakeAvailable(SPIRV-Cross)
#target_link_libraries     (skshaderc PRIVATE
#    spirv-cross-hlsl)

# For SPIRV-Tools
FetchContent_Declare( SPIRV-Headers
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
	GIT_TAG        ${VULKAN_VERSION}
	GIT_SHALLOW    TRUE
	GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(SPIRV-Headers)
target_link_libraries     (skshaderc PRIVATE SPIRV)

# For optimizing SPIR-V shaders, a baseline amount of
# optimization is crucial for meta compatability with
# HLSL compilers.
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
target_link_libraries     (skshaderc PRIVATE SPIRV-Tools-opt)

# For grabbing shader metadata, SPIRV-Cross can also do
# this, but Reflect is lighter weight.
FetchContent_Declare( SPIRV-Reflect
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Reflect.git
	GIT_TAG        ${VULKAN_VERSION}
	GIT_SHALLOW    TRUE
	GIT_PROGRESS   TRUE
)
set(SPIRV_REFLECT_EXECUTABLE OFF CACHE BOOL "")
set(SPIRV_REFLECT_STATIC_LIB ON CACHE BOOL "")
FetchContent_MakeAvailable(SPIRV-Reflect)
target_link_libraries     (skshaderc PRIVATE spirv-reflect-static)

# This is used to convert HLSL to SPIR-V. DXC is an
# alternative, but is far more difficult to compile,
# and generates a much larger binary.
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
target_link_libraries     (skshaderc PRIVATE glslang)

target_sources(skshaderc PRIVATE
    main.cpp
    sksc.cpp
    sksc_hlsl.cpp
    sksc_meta.cpp
    sksc_log.cpp
    sksc.h
    miniz.cpp
    miniz.h
    ../sk_renderer/sksc_file.c
)
target_include_directories(skshaderc PRIVATE ../sk_renderer/include)

set(SKSHADERC_EXE_NAME "skshaderc_exe")
set_target_properties(skshaderc PROPERTIES OUTPUT_NAME ${SKSHADERC_EXE_NAME})
set_target_properties(skshaderc PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION TRUE
)

#if (NOT MSVC AND NOT APPLE)
#    add_custom_command(TARGET skshaderc POST_BUILD
#        COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:skshaderc> $<TARGET_FILE_DIR:skshaderc>/$<TARGET_FILE_BASE_NAME:skshaderc>.dbg
#        COMMAND ${CMAKE_STRIP} --strip-debug $<TARGET_FILE:skshaderc>
#        COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink=$<TARGET_FILE_DIR:skshaderc>/$<TARGET_FILE_BASE_NAME:skshaderc>.dbg $<TARGET_FILE:skshaderc>
#    )
#endif()

set(SKSHADERC_EXE_PATH $<TARGET_FILE:skshaderc> PARENT_SCOPE)

# functions for compiling shaders easily
function(SKSHADERC_COMPILE_ASSETS TARGET OUT_PATH COMMAND_STRING OUT_LIST)
    message(STATUS "sk_gpu compiling shader assets with args '${SKSHADERC_EXE_PATH} ${COMMAND_STRING}'")
    set(SKSHADERC_COMPILE_COMMANDS "-o ${OUT_PATH} -e ${COMMAND_STRING}")
    separate_arguments(SKSHADERC_COMPILE_COMMANDS)

    set(SHADER_LIST)
    set(COMPILED_SHADERS)

    foreach(SHADER IN LISTS ARGN)
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(COMPILED_SHADER "${OUT_PATH}/${SHADER_NAME}.sks")

        add_custom_command(
            OUTPUT ${COMPILED_SHADER}
            COMMAND ${SKSHADERC_EXE_PATH} ${SKSHADERC_COMPILE_COMMANDS} ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}
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
    set(SKSHADERC_COMPILE_COMMANDS "-h -o ${OUTPUT_FOLDER} ${COMMAND_STRING}")
    message(STATUS "sk_gpu compiling shader headers with args '${SKSHADERC_EXE_PATH} ${SKSHADERC_COMPILE_COMMANDS}'")
    separate_arguments(SKSHADERC_COMPILE_COMMANDS)

    set(SHADER_LIST)
    foreach(SHADER IN LISTS ARGN)
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        add_custom_command(
            OUTPUT ${OUTPUT_FOLDER}/${SHADER_NAME}.h
            COMMAND ${SKSHADERC_EXE_PATH} ${SKSHADERC_COMPILE_COMMANDS} ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}
            VERBATIM
        )
        list(APPEND SHADER_LIST ${OUTPUT_FOLDER}/${SHADER_NAME}.h)
    endforeach(SHADER)

    target_include_directories(${ADD_TARGET} PRIVATE ${OUTPUT_FOLDER})
    target_sources            (${ADD_TARGET} PRIVATE ${SHADER_LIST})
endfunction()