cmake_minimum_required(VERSION 3.20)

project(memalloc
    VERSION 0.1.0
    DESCRIPTION "Custom hybrid Slab + TLSF memory allocator"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for editors / clang-tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MEMALLOC_BUILD_TESTS "Build unit tests" ON)
option(MEMALLOC_BUILD_BENCH "Build benchmarks" ON)

# Strict warnings for our own targets only (not fetched deps).
add_library(memalloc_warnings INTERFACE)
target_compile_options(memalloc_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Werror)

# ---------------------------------------------------------------------------
# Core library (empty for Phase 0 — just proves the build wiring works)
# ---------------------------------------------------------------------------
add_library(memalloc STATIC
    src/memalloc.cpp
    src/arena.cpp
    src/slab.cpp
    src/tlsf.cpp)

target_include_directories(memalloc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(memalloc PRIVATE memalloc_warnings)

# ---------------------------------------------------------------------------
# Tests & benchmarks
# ---------------------------------------------------------------------------
if(MEMALLOC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MEMALLOC_BUILD_BENCH)
    add_subdirectory(bench)
endif()
