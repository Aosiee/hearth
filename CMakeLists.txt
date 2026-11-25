# Hearth - the base an application stands on: the host seam and the
# platform (windows, events, input) today; the frame loop and the GPU layer
# as they arrive. This file is the standalone build and what a host's CMake
# build adds with add_subdirectory.
cmake_minimum_required(VERSION 3.20)

# The version is set once, in the header.
file(STRINGS include/hearth/Version.h HEARTH_VERSION_LINE REGEX "^#define HEARTH_VERSION_STRING ")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" HEARTH_VERSION_FROM_HEADER "${HEARTH_VERSION_LINE}")
project(hearth VERSION ${HEARTH_VERSION_FROM_HEADER} LANGUAGES C CXX)

if(CMAKE_VERSION VERSION_LESS 3.21)
    string(COMPARE EQUAL "${CMAKE_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}" PROJECT_IS_TOP_LEVEL)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

option(HEARTH_BUILD_TESTS "Build the suite" ${PROJECT_IS_TOP_LEVEL})
option(HEARTH_NO_ZONES "Compile the profiler zones out" OFF)
# address | undefined | thread: the sanitizer the library and the suite build with.
set(HEARTH_SANITIZE "" CACHE STRING "Build with a sanitizer (address, undefined or thread)")
set(HEARTH_HAMR_DIR "" CACHE PATH "A hamr checkout, for the suite's test library (empty: fetch the pinned commit)")
set(HEARTH_HAMR_COMMIT a4d43e9ffe9c084955d28aab5d77c71ff2c8f597) # hamr 0.1.0 (v0.1.0)
# The platform's window system and its Vulkan surface types.
set(HEARTH_GLFW_DIR "" CACHE PATH "A GLFW checkout (empty: fetch the pinned commit of TinyEngine's fork)")
set(HEARTH_GLFW_COMMIT 21b45cdfe6547987a711673982cb10e0905f0722)
set(HEARTH_VULKAN_HEADERS_DIR "" CACHE PATH "A Vulkan-Headers checkout (empty: fetch the pinned tag)")
set(HEARTH_VULKAN_HEADERS_TAG v1.4.354)
# Only GLFW's null platform: no X11 or Wayland libraries needed, and no
# display to run the suite (CI).
option(HEARTH_NULL_PLATFORM_ONLY "Build GLFW with its null platform only" OFF)
# The GPU layer's developer tools: validation layers, object names, debug
# labels and GPU timestamps. A release program turns them off.
option(HEARTH_GPU_DEBUG "Build the GPU layer's developer tools" ON)
# The GPU layer's libraries, each a checkout or fetched at its pin: volk
# (Vulkan loaded at run time), the Vulkan Memory Allocator, loom (the
# submit threads and record jobs), BLAKE3 (the pipeline cache file's
# checksum) and SPIRV-Cross (shader reflection).
set(HEARTH_VOLK_DIR "" CACHE PATH "A volk checkout (empty: fetch the pinned tag)")
set(HEARTH_VOLK_TAG vulkan-sdk-1.4.328.0)
set(HEARTH_VMA_DIR "" CACHE PATH "A VulkanMemoryAllocator checkout (empty: fetch the pinned tag)")
set(HEARTH_VMA_TAG v3.2.1)
set(HEARTH_LOOM_DIR "" CACHE PATH "A loom checkout (empty: fetch the pinned commit)")
set(HEARTH_LOOM_COMMIT 83f9b4468b19d3b1427db3bcb14ff144440e2882)
set(HEARTH_BLAKE3_DIR "" CACHE PATH "A BLAKE3 checkout (empty: fetch the pinned tag)")
set(HEARTH_BLAKE3_TAG 1.5.4)
set(HEARTH_SPIRV_CROSS_DIR "" CACHE PATH "A SPIRV-Cross checkout (empty: fetch the pinned tag)")
set(HEARTH_SPIRV_CROSS_TAG vulkan-sdk-1.4.328.0)
# stb_image_write, for hearth::app's capture.
set(HEARTH_STB_DIR "" CACHE PATH "An stb checkout (empty: fetch the pinned commit)")
set(HEARTH_STB_COMMIT 6b5799bae8a1fe3792691ef4b72ae26434c3e799)

set(HEARTH_SANITIZE_FLAGS "")
if(HEARTH_SANITIZE)
    set(HEARTH_SANITIZE_FLAGS -fsanitize=${HEARTH_SANITIZE} -fno-omit-frame-pointer)
endif()

function(hearth_flags target)
    if(PROJECT_IS_TOP_LEVEL AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    if(HEARTH_SANITIZE_FLAGS)
        target_compile_options(${target} PRIVATE ${HEARTH_SANITIZE_FLAGS})
        target_link_options(${target} PRIVATE ${HEARTH_SANITIZE_FLAGS})
    endif()
endfunction()

# hearth::core - the host seam, the log, assert and zone macros, and the
# frame loop. Asserts are checked in Debug builds; zones are on unless
# HEARTH_NO_ZONES.
add_library(hearth_core STATIC src/core/Loop.cpp)
add_library(hearth::core ALIAS hearth_core)
target_include_directories(hearth_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hearth_core PUBLIC cxx_std_20)
target_compile_definitions(hearth_core PUBLIC $<$<CONFIG:Debug>:HEARTH_ENABLE_ASSERTS>)
if(NOT HEARTH_NO_ZONES)
    target_compile_definitions(hearth_core PUBLIC HEARTH_ENABLE_ZONES)
endif()
hearth_flags(hearth_core)

# hearth::platform - windows, events, input, the clipboard, gamepads and
# Vulkan surfaces, on GLFW (the only Hearth code that includes it).
if(NOT TARGET glfw)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    if(HEARTH_NULL_PLATFORM_ONLY)
        set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    endif()
    if(HEARTH_GLFW_DIR)
        add_subdirectory(${HEARTH_GLFW_DIR} ${CMAKE_BINARY_DIR}/glfw EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/Aosiee/te-glfw-fork.git GIT_TAG ${HEARTH_GLFW_COMMIT})
        FetchContent_MakeAvailable(glfw)
    endif()
endif()
if(HEARTH_VULKAN_HEADERS_DIR)
    set(HEARTH_VULKAN_INCLUDE "${HEARTH_VULKAN_HEADERS_DIR}/include")
else()
    FetchContent_Declare(vulkan_headers GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG ${HEARTH_VULKAN_HEADERS_TAG} SOURCE_SUBDIR hearth-headers-only)
    FetchContent_MakeAvailable(vulkan_headers)
    set(HEARTH_VULKAN_INCLUDE "${vulkan_headers_SOURCE_DIR}/include")
endif()

add_library(hearth_platform STATIC src/platform/Platform.cpp src/platform/Window.cpp)
add_library(hearth::platform ALIAS hearth_platform)
target_link_libraries(hearth_platform PUBLIC hearth_core PRIVATE glfw)
target_include_directories(hearth_platform PRIVATE "${HEARTH_VULKAN_INCLUDE}")
hearth_flags(hearth_platform)

# A library's sources, from a checkout or fetched at its pin, without its
# own CMake build (SOURCE_SUBDIR names a directory without a
# CMakeLists.txt).
function(hearth_source_of name url tag dir_var out_var)
    if(${dir_var})
        set(${out_var} "${${dir_var}}" PARENT_SCOPE)
        return()
    endif()
    FetchContent_Declare(${name} GIT_REPOSITORY ${url} GIT_TAG ${tag} SOURCE_SUBDIR hearth-sources-only)
    FetchContent_MakeAvailable(${name})
    set(${out_var} "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

hearth_source_of(volk https://github.com/zeux/volk.git ${HEARTH_VOLK_TAG} HEARTH_VOLK_DIR HEARTH_VOLK_SOURCE)
hearth_source_of(vma https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git ${HEARTH_VMA_TAG} HEARTH_VMA_DIR HEARTH_VMA_SOURCE)
hearth_source_of(blake3 https://github.com/BLAKE3-team/BLAKE3.git ${HEARTH_BLAKE3_TAG} HEARTH_BLAKE3_DIR HEARTH_BLAKE3_SOURCE)
hearth_source_of(spirv_cross https://github.com/KhronosGroup/SPIRV-Cross.git ${HEARTH_SPIRV_CROSS_TAG} HEARTH_SPIRV_CROSS_DIR HEARTH_SPIRV_CROSS_SOURCE)
# loom builds with its own CMake; a host that has it keeps its own.
if(NOT TARGET loom)
    if(HEARTH_LOOM_DIR)
        add_subdirectory(${HEARTH_LOOM_DIR} ${CMAKE_BINARY_DIR}/loom EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare(loom GIT_REPOSITORY https://github.com/Aosiee/loom.git GIT_TAG ${HEARTH_LOOM_COMMIT})
        FetchContent_MakeAvailable(loom)
    endif()
endif()

add_library(hearth_volk STATIC "${HEARTH_VOLK_SOURCE}/volk.c")
target_include_directories(hearth_volk SYSTEM PUBLIC "${HEARTH_VOLK_SOURCE}" "${HEARTH_VULKAN_INCLUDE}" "${HEARTH_VMA_SOURCE}/include")
target_link_libraries(hearth_volk PRIVATE ${CMAKE_DL_LIBS})

# The portable implementation only, as TinyEngine builds it: the checksum
# covers a pipeline cache file of a few megabytes, once per run.
add_library(hearth_blake3 STATIC "${HEARTH_BLAKE3_SOURCE}/c/blake3.c" "${HEARTH_BLAKE3_SOURCE}/c/blake3_dispatch.c" "${HEARTH_BLAKE3_SOURCE}/c/blake3_portable.c")
target_include_directories(hearth_blake3 SYSTEM PUBLIC "${HEARTH_BLAKE3_SOURCE}/c")
target_compile_definitions(hearth_blake3 PRIVATE BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)

add_library(hearth_spirv_cross STATIC "${HEARTH_SPIRV_CROSS_SOURCE}/spirv_cross.cpp" "${HEARTH_SPIRV_CROSS_SOURCE}/spirv_parser.cpp"
    "${HEARTH_SPIRV_CROSS_SOURCE}/spirv_cross_parsed_ir.cpp" "${HEARTH_SPIRV_CROSS_SOURCE}/spirv_cfg.cpp")
target_include_directories(hearth_spirv_cross SYSTEM PUBLIC "${HEARTH_SPIRV_CROSS_SOURCE}")

# hearth::rhi - the GPU layer: the RHI interface and the render graph, and
# their Vulkan implementation (the device, swapchains, pipelines, shaders,
# buffers, images, the bindless set, the retire queue).
file(GLOB HEARTH_RHI_SOURCES CONFIGURE_DEPENDS src/rhi/*.cpp src/rhi/vulkan/*.cpp)
add_library(hearth_rhi STATIC ${HEARTH_RHI_SOURCES})
add_library(hearth::rhi ALIAS hearth_rhi)
target_link_libraries(hearth_rhi PUBLIC hearth_core hearth_platform hearth_volk loom PRIVATE hearth_blake3 hearth_spirv_cross)
if(HEARTH_GPU_DEBUG)
    target_compile_definitions(hearth_rhi PUBLIC HEARTH_GPU_DEBUG)
endif()
hearth_flags(hearth_rhi)

# hearth::app - the loop's owner for a program that is not an engine: the
# platform, the main window, a thread pool, the device, the main window's
# presenter and the capture flags (the PNG through stb_image_write, the
# sidecar's hash through BLAKE3).
hearth_source_of(stb https://github.com/nothings/stb.git ${HEARTH_STB_COMMIT} HEARTH_STB_DIR HEARTH_STB_SOURCE)
add_library(hearth_app STATIC src/app/App.cpp)
add_library(hearth::app ALIAS hearth_app)
target_include_directories(hearth_app SYSTEM PRIVATE "${HEARTH_STB_SOURCE}")
target_link_libraries(hearth_app PUBLIC hearth_core hearth_platform hearth_rhi PRIVATE hearth_blake3)
hearth_flags(hearth_app)

# hearth::default_host - the host for a program without one of its own. An
# object library: the libraries call into it, so as an archive a linker that
# reads in order (GNU ld) would drop it when it came before them.
add_library(hearth_default_host OBJECT src/default_host/DefaultHost.cpp)
add_library(hearth::default_host ALIAS hearth_default_host)
target_link_libraries(hearth_default_host PUBLIC hearth_core)
hearth_flags(hearth_default_host)

if(HEARTH_BUILD_TESTS)
    if(HEARTH_HAMR_DIR)
        set(HEARTH_HAMR_SOURCE "${HEARTH_HAMR_DIR}")
    else()
        # Only hamr's header is needed: SOURCE_SUBDIR names a directory
        # without a CMakeLists.txt, so hamr's own build does not run.
        FetchContent_Declare(hamr GIT_REPOSITORY https://github.com/Aosiee/hamr.git GIT_TAG ${HEARTH_HAMR_COMMIT} SOURCE_SUBDIR hearth-headers-only)
        FetchContent_MakeAvailable(hamr)
        set(HEARTH_HAMR_SOURCE "${hamr_SOURCE_DIR}")
    endif()

    add_executable(hearth_tests tests/main.cpp tests/TestHost.cpp tests/HostTests.cpp tests/DisabledTests.cpp tests/LoopTests.cpp)
    target_include_directories(hearth_tests PRIVATE tests "${HEARTH_HAMR_SOURCE}/include")
    target_link_libraries(hearth_tests PRIVATE hearth_core)
    hearth_flags(hearth_tests)

    add_executable(hearth_platform_tests tests/main.cpp tests/PlatformTests.cpp tests/KeysTests.cpp)
    target_include_directories(hearth_platform_tests PRIVATE "${HEARTH_HAMR_SOURCE}/include")
    target_link_libraries(hearth_platform_tests PRIVATE hearth_platform hearth_default_host glfw)
    hearth_flags(hearth_platform_tests)

    # The GPU layer's cases that need no GPU: the render graph, the
    # pipeline cache file, the descriptor write log.
    add_executable(hearth_rhi_tests tests/main.cpp tests/RenderGraphTests.cpp tests/PipelineCacheFileTests.cpp tests/DescriptorLogUtilTests.cpp
        tests/AppArgsTests.cpp)
    target_include_directories(hearth_rhi_tests PRIVATE "${HEARTH_HAMR_SOURCE}/include")
    target_link_libraries(hearth_rhi_tests PRIVATE hearth_app hearth_default_host)
    hearth_flags(hearth_rhi_tests)

    # The GPU layer on a device (lavapipe in CI): an offscreen device, a
    # compute dispatch read back, the bindless retire, an offscreen
    # presenter and its clients.
    add_executable(hearth_rhi_gpu_tests tests/main.cpp tests/RhiGpuTests.cpp tests/PresenterGpuTests.cpp tests/AppGpuTests.cpp)
    target_include_directories(hearth_rhi_gpu_tests PRIVATE tests "${HEARTH_HAMR_SOURCE}/include")
    target_link_libraries(hearth_rhi_gpu_tests PRIVATE hearth_app hearth_default_host)
    hearth_flags(hearth_rhi_gpu_tests)

    add_executable(hearth_default_host_tests tests/DefaultHostTests.cpp)
    target_include_directories(hearth_default_host_tests PRIVATE "${HEARTH_HAMR_SOURCE}/include")
    target_link_libraries(hearth_default_host_tests PRIVATE hearth_default_host)
    hearth_flags(hearth_default_host_tests)
endif()
