# SPDX-License-Identifier: MIT
#
# fastsbc_hmr — a production-grade compiler that lowers Oracle HMR (Header
# Manipulation Rules) DSL directly to native .so modules via LLVM IR.
#
# Build layout:
#   hmr_core     front-end + optimizer + runtime model (NO LLVM dependency)
#   hmr_codegen  LLVM IR generator + backend + module loader + pipeline facade
#                (built only when LLVM is found; guarded by HMR_ENABLE_LLVM)
#   hmrc         command-line driver
#   tests/       unit + integration tests (self-contained harness)
#   benchmarks/  micro-benchmarks for the generated fast path

cmake_minimum_required(VERSION 3.24)

project(fastsbc_hmr
  VERSION 0.1.0
  DESCRIPTION "Oracle HMR DSL -> native .so compiler (direct LLVM IR)"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Standard / global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)            # C++26 features are shimmed in Compat.hpp
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(HMR_ENABLE_LLVM "Build the LLVM-backed codegen/backend layers" ON)
option(HMR_BUILD_TESTS "Build the test suite" ON)
option(HMR_BUILD_BENCHMARKS "Build micro-benchmarks" ON)
option(HMR_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

add_library(hmr_warnings INTERFACE)
target_compile_options(hmr_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(HMR_WARNINGS_AS_ERRORS)
  target_compile_options(hmr_warnings INTERFACE -Werror)
endif()

# ---------------------------------------------------------------------------
# hmr_core — the LLVM-free front-end. Everything here is unit-testable without
# any heavy dependency, which keeps the inner development loop fast.
# ---------------------------------------------------------------------------
add_library(hmr_core STATIC
  src/parser/Lexer.cpp
  src/parser/Parser.cpp
  src/ast/Value.cpp
  src/ast/AstFactory.cpp
  src/ast/AstPrinter.cpp
  src/optimizer/Optimizer.cpp
  src/runtime/SipMessage.cpp
  src/runtime/Runtime.cpp
)
target_include_directories(hmr_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(hmr_core PRIVATE hmr_warnings)

# ---------------------------------------------------------------------------
# LLVM detection
# ---------------------------------------------------------------------------
set(HMR_HAVE_LLVM OFF)
if(HMR_ENABLE_LLVM)
  find_package(LLVM CONFIG QUIET)
  if(LLVM_FOUND)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_INCLUDE_DIRS})")
    set(HMR_HAVE_LLVM ON)
  else()
    message(WARNING
      "LLVM not found; building front-end only. "
      "Install llvm-dev (>=17) and re-run, or pass -DHMR_ENABLE_LLVM=OFF.")
  endif()
endif()

# ---------------------------------------------------------------------------
# hmr_codegen — LLVM IR generation + object emission + linking + hot reload.
# ---------------------------------------------------------------------------
if(HMR_HAVE_LLVM)
  llvm_map_components_to_libnames(HMR_LLVM_LIBS
    core support irreader passes analysis
    target nativecodegen native mc object bitwriter)

  add_library(hmr_codegen STATIC
    src/codegen/IrGenerator.cpp
    src/backend/Backend.cpp
    src/backend/Linker.cpp
    src/module/ModuleManager.cpp
    src/pipeline/Compiler.cpp
  )
  target_include_directories(hmr_codegen
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  # SYSTEM must be its own call: as a mid-list keyword CMake treats it as a path.
  # Marking LLVM's headers system silences their (copious) -W noise in our build.
  target_include_directories(hmr_codegen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(hmr_codegen PRIVATE ${LLVM_DEFINITIONS} HMR_HAVE_LLVM=1)
  target_compile_definitions(hmr_codegen PUBLIC HMR_HAVE_LLVM=1)
  target_link_libraries(hmr_codegen
    PUBLIC hmr_core ${CMAKE_DL_LIBS}
    PRIVATE hmr_warnings ${HMR_LLVM_LIBS})
endif()

# ---------------------------------------------------------------------------
# hmrc — the compiler driver.
# ---------------------------------------------------------------------------
add_executable(hmrc src/tools/hmrc.cpp)
target_link_libraries(hmrc PRIVATE hmr_core hmr_warnings ${CMAKE_DL_LIBS})
if(HMR_HAVE_LLVM)
  target_link_libraries(hmrc PRIVATE hmr_codegen)
  target_compile_definitions(hmrc PRIVATE HMR_HAVE_LLVM=1)
endif()

# ---------------------------------------------------------------------------
# Tests / benchmarks
# ---------------------------------------------------------------------------
if(HMR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(HMR_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS hmrc RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
