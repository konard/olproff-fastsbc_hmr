# SPDX-License-Identifier: MIT
#
# Builds the host-integration example. Works two ways:
#   * in-tree   — added via add_subdirectory() from the top-level CMakeLists
#                 (the hmr_core / hmr_codegen targets already exist), or
#   * standalone — `cmake -S examples/host_integration -B build-example`, which
#                 pulls the compiler libraries in from the repository root.

cmake_minimum_required(VERSION 3.24)
project(hmr_host_integration CXX)

# Standalone configure: bring in the compiler from the repo root (tests,
# benchmarks and examples turned off to avoid building them / recursing).
if(NOT TARGET hmr_codegen)
  set(HMR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(HMR_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(HMR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_BINARY_DIR}/fastsbc_hmr)
endif()

if(NOT TARGET hmr_codegen)
  message(FATAL_ERROR
    "host_integration needs the LLVM-backed codegen (hmr_codegen). Configure "
    "with -DHMR_ENABLE_LLVM=ON and an installed LLVM (>=17).")
endif()

add_executable(hmr_host_integration main.cpp)
target_compile_features(hmr_host_integration PRIVATE cxx_std_23)
target_link_libraries(hmr_host_integration
  PRIVATE hmr_core hmr_codegen ${CMAKE_DL_LIBS})

# The generated .so resolves the hmr_rt_* callbacks against THIS process at
# dlopen time, so the host must export its dynamic symbols. Calling makeContext()
# pulls the runtime object (which defines hmr_rt_*) into the link.
set_target_properties(hmr_host_integration PROPERTIES ENABLE_EXPORTS ON)
