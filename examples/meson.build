# SPDX-License-Identifier: MIT
#
# host_integration — end-to-end embedding example: compile an embedded ruleset to
# a native .so, load it, apply it to a SIP message, and hot-swap it. Needs the
# full LLVM pipeline (compile), the module loader (dlopen) and the runtime.
#
# Only reached when enable_llvm is set (gated by the root meson.build). Like the
# codegen tests, it must export its dynamic symbols so the dlopen'd module
# resolves its hmr_rt_* imports against this process; referencing make_context
# force-links the runtime TU that defines them.

example_host = executable('example_host_integration',
  'host_integration/main.cpp',
  dependencies: [hmr_pipeline_dep, hmr_backend_dep, hmr_module_dep,
                 hmr_runtime_dep],
  cpp_args: hmr_warn_args,
  export_dynamic: true)
