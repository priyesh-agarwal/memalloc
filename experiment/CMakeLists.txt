cmake_minimum_required(VERSION 3.20)
project(memalloc_experiment LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -fno-builtin)

# These binaries call malloc. They are not linked to an allocator.
# Point MEMALLOC_PRELOAD at libmemalloc.so, libtcmalloc.so.4, or libjemalloc.so.2.
set(MEMALLOC_PRELOAD "${CMAKE_CURRENT_SOURCE_DIR}/../build/libmemalloc.so"
    CACHE FILEPATH "Shared library passed to LD_PRELOAD")

add_executable(test_alloc test_alloc.cpp)
add_executable(memalloc_bench bench.cpp)

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
