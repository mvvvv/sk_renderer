# Custom FindVulkan.cmake for cross-compilation
# Only requires Vulkan headers (not the library) since we use volk for loading
#
# This module sets:
#   Vulkan_FOUND        - TRUE if headers are found
#   Vulkan_INCLUDE_DIRS - Include directories
#   Vulkan::Headers     - Imported target for headers

# Check if Vulkan_INCLUDE_DIR is already set (e.g., from FetchContent)
if(Vulkan_INCLUDE_DIR)
    # Verify headers actually exist
    if(EXISTS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan.h")
        set(Vulkan_FOUND TRUE)
        set(Vulkan_INCLUDE_DIRS "${Vulkan_INCLUDE_DIR}")

        # Create imported target if it doesn't exist
        if(NOT TARGET Vulkan::Headers)
            add_library(Vulkan::Headers INTERFACE IMPORTED)
            set_target_properties(Vulkan::Headers PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}"
            )
        endif()

        # Extract version from vulkan_core.h
        if(EXISTS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan_core.h")
            file(STRINGS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan_core.h" _vulkan_version_line
                 REGEX "^#define VK_HEADER_VERSION_COMPLETE ")
            if(_vulkan_version_line)
                string(REGEX MATCH "VK_MAKE_API_VERSION\\([^,]+,[ ]*([0-9]+),[ ]*([0-9]+),[ ]*([0-9]+)" _ "${_vulkan_version_line}")
                set(Vulkan_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
            endif()
        endif()

        message(STATUS "Found Vulkan headers: ${Vulkan_INCLUDE_DIR} (version ${Vulkan_VERSION})")
    else()
        set(Vulkan_FOUND FALSE)
        message(STATUS "Vulkan_INCLUDE_DIR set but vulkan.h not found at: ${Vulkan_INCLUDE_DIR}")
    endif()
else()
    set(Vulkan_FOUND FALSE)
    message(STATUS "Vulkan headers not found (Vulkan_INCLUDE_DIR not set)")
endif()

# Standard CMake package handling
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Vulkan
    REQUIRED_VARS Vulkan_INCLUDE_DIR
    VERSION_VAR Vulkan_VERSION
)
