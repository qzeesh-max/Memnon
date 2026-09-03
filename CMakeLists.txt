cmake_minimum_required(VERSION 3.20)
project(BoostInterprocessGrowingMemorySegment
        VERSION 0.1.0
        DESCRIPTION "Boost.Interprocess extension: online-growable segmented managed memory"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# C++ standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Find dependencies
# ---------------------------------------------------------------------------
# Homebrew prefix (macOS arm64 / intel)
if(APPLE)
    # Homebrew on Apple Silicon installs to /opt/homebrew
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
        list(PREPEND CMAKE_PREFIX_PATH /opt/homebrew)
    else()
        list(PREPEND CMAKE_PREFIX_PATH /usr/local)
    endif()
endif()

find_package(Boost REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Header-only library target
# ---------------------------------------------------------------------------
add_library(segmented_interprocess INTERFACE)
add_library(SegmentedInterprocess::segmented_interprocess
            ALIAS segmented_interprocess)

target_include_directories(segmented_interprocess INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(segmented_interprocess INTERFACE
    Boost::headers
    Threads::Threads
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(segmented_interprocess INTERFACE rt)
endif()

target_compile_options(segmented_interprocess INTERFACE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
    >
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4
    >
)

# ---------------------------------------------------------------------------
# Optional sanitizers
# ---------------------------------------------------------------------------
set(SI_SANITIZE "" CACHE STRING "Sanitizer: address | thread | undefined | (empty = none)")
if(SI_SANITIZE AND NOT SI_SANITIZE STREQUAL "none")
    message(STATUS "Enabling sanitizer: ${SI_SANITIZE}")
    if(MSVC)
        add_compile_options(/fsanitize=${SI_SANITIZE} /Zi)
        add_link_options(/INCREMENTAL:NO)
    else()
        add_compile_options(-fsanitize=${SI_SANITIZE} -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=${SI_SANITIZE})
    endif()
else()
    message(STATUS "No sanitizer enabled")
endif()

# ---------------------------------------------------------------------------
# Google Benchmark (via FetchContent)
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.8.3
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Disable benchmark tests" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "Disable benchmark install" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "Disable benchmark werror" FORCE)
FetchContent_MakeAvailable(benchmark)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
add_subdirectory(benchmarks)
