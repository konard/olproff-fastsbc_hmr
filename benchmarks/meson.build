# SPDX-License-Identifier: MIT
#
# Performance benchmarks (review #5: "Comparison with the interpreter" and
# "Comparison with the GCC approach").
#
#   bench_frontend  — LLVM-free: front-end (parse+optimize) ms/compile and the
#                     interpreter apply baseline. Builds in any configuration.
#   bench_pipeline  — needs the LLVM pipeline + module loader and the C++ "GCC
#                     approach" back-end; reports the compiled figures and both
#                     comparisons. Gated on enable_llvm.
#
# Registered with benchmark() so they build with the project and run under
# `meson test --benchmark` rather than the normal `meson test` (each does
# hundreds of thousands of apply iterations and forks external toolchains).

bench_frontend = executable('bench_frontend', 'bench_frontend.cpp',
  dependencies: [hmr_interp_dep, hmr_optimizer_dep],
  cpp_args: hmr_warn_args)
benchmark('frontend', bench_frontend, timeout: 120)

if enable_llvm
  # measure_apply references make_context, force-linking the runtime TU that
  # defines every hmr_rt_*; export_dynamic publishes them so the dlopen'd
  # modules (LLVM-direct and g++-built alike) resolve against this process.
  # HMR_INCLUDE_DIR tells the generated C++ where to find hmr_runtime.h when
  # the "GCC approach" path invokes g++.
  bench_pipeline = executable('bench_pipeline', 'bench_pipeline.cpp',
    dependencies: [hmr_pipeline_dep, hmr_codegen_dep, hmr_backend_dep,
                   hmr_module_dep, hmr_interp_dep, hmr_cppgen_dep,
                   hmr_runtime_dep],
    cpp_args: hmr_warn_args + [
      '-DHMR_INCLUDE_DIR="' + (meson.project_source_root() / 'include') + '"'],
    export_dynamic: true)
  benchmark('pipeline', bench_pipeline, timeout: 600)
endif
