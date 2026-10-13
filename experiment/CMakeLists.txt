cmake_minimum_required(VERSION 3.20)
project(memalloc_experiment LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# These binaries call malloc. They are not linked to an allocator.
# Point MEMALLOC_PRELOAD at libmemalloc.so, libtcmalloc.so.4, or libjemalloc.so.2.
set(MEMALLOC_PRELOAD "${CMAKE_CURRENT_SOURCE_DIR}/../build/libmemalloc.so"
    CACHE FILEPATH "Shared library passed to LD_PRELOAD")

find_package(benchmark QUIET)
if(NOT benchmark_FOUND)
  include(FetchContent)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(benchmark
      GIT_REPOSITORY https://github.com/google/benchmark.git
      GIT_TAG v1.9.5
      GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(benchmark)
endif()

add_executable(test_alloc test_alloc.cpp)
add_executable(memalloc_bench bench.cpp)
# -fno-builtin keeps malloc/free calls from being folded away. Only on our
# targets, so Google Benchmark keeps its own NDEBUG release build.
target_compile_options(test_alloc PRIVATE -Wall -Wextra -fno-builtin -UNDEBUG)
target_compile_options(memalloc_bench PRIVATE -Wall -Wextra -fno-builtin)
target_link_libraries(memalloc_bench PRIVATE benchmark::benchmark)

add_custom_target(test
    COMMAND ${CMAKE_COMMAND} -E env LD_PRELOAD=${MEMALLOC_PRELOAD}
            $<TARGET_FILE:test_alloc>
    DEPENDS test_alloc
    USES_TERMINAL
    VERBATIM
)

add_custom_target(bench
    COMMAND ${CMAKE_COMMAND} -E env LD_PRELOAD=${MEMALLOC_PRELOAD}
            $<TARGET_FILE:memalloc_bench>
    DEPENDS memalloc_bench
    USES_TERMINAL
    VERBATIM
)

# Fair run is: sudo /bin/sh experiment/compare.sh
# This target does not get SCHED_FIFO, because cmake is not root.
add_custom_target(compare
    COMMAND /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/compare.sh
    DEPENDS memalloc_bench
    USES_TERMINAL
    VERBATIM
)
