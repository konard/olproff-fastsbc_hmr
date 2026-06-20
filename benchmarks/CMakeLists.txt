# Micro-benchmarks. Dependency-free (std::chrono), so they build without any
# third-party benchmark library. The front-end/runtime benchmark always builds;
# the end-to-end compiled-module benchmark builds only with LLVM.

add_executable(hmr_bench_frontend bench_frontend.cpp)
target_link_libraries(hmr_bench_frontend PRIVATE hmr_core)

if(HMR_HAVE_LLVM)
    add_executable(hmr_bench_pipeline bench_pipeline.cpp)
    target_link_libraries(hmr_bench_pipeline PRIVATE hmr_core hmr_codegen ${CMAKE_DL_LIBS})
    target_compile_definitions(hmr_bench_pipeline PRIVATE HMR_HAVE_LLVM=1)
    # Export hmr_rt_* so the dlopen'd benchmark module resolves them at load time.
    set_target_properties(hmr_bench_pipeline PROPERTIES ENABLE_EXPORTS ON)
endif()
