# Unit tests for fastsbc_hmr. The harness (tests/Test.hpp) is dependency-free:
# every TEST() self-registers into a shared registry, so a single executable
# links all translation units and runs the whole suite under ctest.

set(HMR_TEST_SOURCES
    test_main.cpp
    test_value.cpp
    test_lexer.cpp
    test_parser.cpp
    test_optimizer.cpp
    test_runtime.cpp
)

# Codegen / pipeline integration test only builds when LLVM is available.
if(HMR_HAVE_LLVM)
    list(APPEND HMR_TEST_SOURCES test_codegen.cpp)
endif()

add_executable(hmr_tests ${HMR_TEST_SOURCES})
target_link_libraries(hmr_tests PRIVATE hmr_core hmr_warnings)
target_include_directories(hmr_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(HMR_HAVE_LLVM)
    target_link_libraries(hmr_tests PRIVATE hmr_codegen)
    target_compile_definitions(hmr_tests PRIVATE HMR_HAVE_LLVM=1)
endif()

add_test(NAME hmr_tests COMMAND hmr_tests)
set_tests_properties(hmr_tests PROPERTIES TIMEOUT 120)

# Smoke-test that every bundled sample parses (or fails, for invalid_*).
add_test(
    NAME hmr_samples
    COMMAND hmrc check-samples ${CMAKE_CURRENT_SOURCE_DIR}/hmr_samples
)
set_tests_properties(hmr_samples PROPERTIES TIMEOUT 120)
