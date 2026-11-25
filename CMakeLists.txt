# Hearth - the base an application stands on: the host seam today; the
# platform (windows, events, input), the frame loop and the GPU layer as
# they arrive. This file is the standalone build and what a host's CMake
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
set(HEARTH_HAMR_COMMIT 58f44868db9a1b0e4ef22c412ebfd11b20b5a223)

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

    add_executable(hearth_default_host_tests tests/DefaultHostTests.cpp)
    target_include_directories(hearth_default_host_tests PRIVATE "${HEARTH_HAMR_SOURCE}/include")
    target_link_libraries(hearth_default_host_tests PRIVATE hearth_default_host)
    hearth_flags(hearth_default_host_tests)
endif()
