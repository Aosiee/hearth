# Hearth - the base an application stands on: the host seam and the
# platform (windows, events, input) today; the frame loop and the GPU layer
# as they arrive. This file is the standalone build and what a host's CMake
# build adds with add_subdirectory.
cmake_minimum_required(VERSION 3.20)

# The version is set once, in the header.
file(STRINGS include/hearth/Version.h HEARTH_VERSION_LINE REGEX "^#define HEARTH_VERSION_STRING ")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" HEARTH_VERSION_FROM_HEADER "${HEARTH_VERSION_LINE}")
project(hearth VERSION ${HEARTH_VERSION_FROM_HEADER} LANGUAGES CXX)

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

# hearth::core - the host seam and the log, assert and zone macros. Asserts
# are checked in Debug builds; zones are on unless HEARTH_NO_ZONES.
add_library(hearth_core INTERFACE)
add_library(hearth::core ALIAS hearth_core)
target_include_directories(hearth_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hearth_core INTERFACE cxx_std_20)
target_compile_definitions(hearth_core INTERFACE $<$<CONFIG:Debug>:HEARTH_ENABLE_ASSERTS>)
if(NOT HEARTH_NO_ZONES)
    target_compile_definitions(hearth_core INTERFACE HEARTH_ENABLE_ZONES)
endif()

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

# hearth::default_host - the host for a program without one of its own.
add_library(hearth_default_host STATIC src/default_host/DefaultHost.cpp)
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

    add_executable(hearth_tests tests/main.cpp tests/TestHost.cpp tests/HostTests.cpp tests/DisabledTests.cpp)
    target_include_directories(hearth_tests PRIVATE tests "${HEARTH_HAMR_SOURCE}/include")
    target_link_libraries(hearth_tests PRIVATE hearth_core)
    hearth_flags(hearth_tests)

    add_executable(hearth_platform_tests tests/main.cpp tests/PlatformTests.cpp tests/KeysTests.cpp)
    target_include_directories(hearth_platform_tests PRIVATE "${HEARTH_HAMR_SOURCE}/include")
    target_link_libraries(hearth_platform_tests PRIVATE hearth_platform hearth_default_host glfw)
    hearth_flags(hearth_platform_tests)

    add_executable(hearth_default_host_tests tests/DefaultHostTests.cpp)
    target_include_directories(hearth_default_host_tests PRIVATE "${HEARTH_HAMR_SOURCE}/include")
    target_link_libraries(hearth_default_host_tests PRIVATE hearth_default_host)
    hearth_flags(hearth_default_host_tests)
endif()
