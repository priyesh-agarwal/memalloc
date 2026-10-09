cmake_minimum_required(VERSION 3.20)
project(memalloc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The build type must not change this library. Ubuntu's libtcmalloc.so.4
# (google-perftools 2.18.1-1) and libjemalloc.so.2 (jemalloc 5.3.0-4) are
# compiled with dpkg-buildflags, not with -O3 or -march=native. Those flags
# are reproduced below. -DNDEBUG is not one of them. -fno-builtin keeps
# malloc a real function, which those libraries do as well.
set(CMAKE_CXX_FLAGS "" CACHE STRING "" FORCE)
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_CXX_FLAGS_${cfg} "" CACHE STRING "" FORCE)
endforeach()

add_library(memalloc SHARED malloc.cpp)
target_compile_options(memalloc PRIVATE
    -Wall
    -Wextra
    -O2
    -g
    -fno-builtin
    -fno-omit-frame-pointer
    -mno-omit-leaf-frame-pointer
    -fstack-protector-strong
    -fstack-clash-protection
    -fcf-protection
    -flto=auto
    -ffat-lto-objects
)
target_link_options(memalloc PRIVATE
    -flto=auto
    -ffat-lto-objects
    -Wl,-Bsymbolic-functions
    -Wl,-z,relro
    -fstack-protector-strong
    -fcf-protection
)
