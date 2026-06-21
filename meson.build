# SPDX-License-Identifier: MIT
#
# fastsbc_hmr — Oracle HMR (Header Manipulation Rules) DSL -> native .so
# compiler that lowers directly to LLVM IR.
#
# Build layout (grows per milestone):
#   hmr_antlr     ANTLR4-generated lexer/parser (compiled warning-free)
#   hmr_frontend  snake_case front-end: grammar visitor + AST + value folding
#   tests/        per-sample parse tests + unit suite
#
# ANTLR4 is not packaged on most distros, so the generator jar and the C++
# runtime are located via build options (see meson.options). scripts/
# setup_antlr.sh fetches a pinned copy and prints the three paths to pass.

project('fastsbc_hmr', 'cpp',
  version: '0.2.0',
  license: 'MIT',
  meson_version: '>=1.1.0',
  default_options: [
    'cpp_std=c++23',          # C++26 features are shimmed in support/compat.hpp
    'warning_level=2',
    'default_library=static',
    'buildtype=release',
    'b_ndebug=if-release',
  ])

cpp = meson.get_compiler('cpp')

# Extra warnings layered on top of warning_level=2 for OUR code only. The
# ANTLR-generated translation units are compiled separately with warnings off.
hmr_warn_args = cpp.get_supported_arguments(
  '-Wshadow', '-Wconversion', '-Wsign-conversion', '-Wpedantic')

hmr_inc = include_directories('include')

# ---------------------------------------------------------------------------
# ANTLR4 — generate the parser and link the C++ runtime.
# ---------------------------------------------------------------------------
antlr_jar = get_option('antlr_jar')
antlr_inc = get_option('antlr_inc')
antlr_libdir = get_option('antlr_libdir')

if antlr_jar == ''
  error('option antlr_jar is required, e.g. -Dantlr_jar=/path/antlr-4.13.2-complete.jar (run scripts/setup_antlr.sh)')
endif
if antlr_inc == ''
  error('option antlr_inc is required, e.g. -Dantlr_inc=/path/include/antlr4-runtime')
endif
if antlr_libdir == ''
  error('option antlr_libdir is required, e.g. -Dantlr_libdir=/path/lib')
endif

run_antlr = find_program('scripts/run_antlr.sh')

# One generator invocation produces the lexer, parser and visitor base. The
# wrapper writes them FLAT into @OUTDIR@ (the build dir for this meson.build).
antlr_gen = custom_target('antlr_hmr',
  input: 'grammar/Hmr.g4',
  output: [
    'HmrLexer.cpp', 'HmrLexer.h',
    'HmrParser.cpp', 'HmrParser.h',
    'HmrBaseVisitor.cpp', 'HmrBaseVisitor.h',
    'HmrVisitor.cpp', 'HmrVisitor.h',
  ],
  command: [run_antlr, antlr_jar, '@INPUT@', '@OUTDIR@'])

antlr_gen_cpp = [antlr_gen[0], antlr_gen[2], antlr_gen[4], antlr_gen[6]]
antlr_gen_hdr = [antlr_gen[1], antlr_gen[3], antlr_gen[5], antlr_gen[7]]

# include_directories('.') resolves to the build-dir counterpart too, which is
# where the generated headers land. is_system keeps their warnings out of ours.
antlr_gen_inc = include_directories('.', is_system: true)

# Prefer the static archive; -isystem keeps the runtime's headers warning-free.
antlr_runtime = cpp.find_library('antlr4-runtime',
  dirs: [antlr_libdir], static: true, required: true)
thread_dep = dependency('threads')

antlr_dep = declare_dependency(
  compile_args: ['-isystem', antlr_inc],
  dependencies: [antlr_runtime, thread_dep])

# Generated TUs: no strict warnings, never -Werror.
libhmr_antlr = static_library('hmr_antlr',
  antlr_gen_cpp + antlr_gen_hdr,
  include_directories: antlr_gen_inc,
  dependencies: antlr_dep,
  cpp_args: ['-w'],
  override_options: ['warning_level=0', 'werror=false'])

# ---------------------------------------------------------------------------
# hmr_frontend — snake_case front-end (parser visitor + AST + value folding).
# ---------------------------------------------------------------------------
hmr_frontend_sources = [
  'src/ast/value.cpp',
  'src/ast/ast_factory.cpp',
  'src/ast/ast_printer.cpp',
  'src/parser/parser.cpp',
]

libhmr_frontend = static_library('hmr_frontend',
  hmr_frontend_sources + antlr_gen_hdr,
  include_directories: [hmr_inc, antlr_gen_inc],
  dependencies: antlr_dep,
  link_with: libhmr_antlr,
  cpp_args: hmr_warn_args)

hmr_frontend_dep = declare_dependency(
  include_directories: hmr_inc,
  link_with: [libhmr_frontend, libhmr_antlr],
  dependencies: antlr_dep)

# ---------------------------------------------------------------------------
# hmr_runtime — the thin native runtime every generated .so links against:
# the stable C ABI, the zero-copy arena SIP model and the specialized
# match-val-type matchers. No ANTLR/LLVM dependency; pure C++ + the C ABI.
# ---------------------------------------------------------------------------
hmr_runtime_sources = [
  'src/runtime/matchers.cpp',
  'src/runtime/uri.cpp',
  'src/runtime/sip_message.cpp',
  'src/runtime/runtime.cpp',
]

libhmr_runtime = static_library('hmr_runtime',
  hmr_runtime_sources,
  include_directories: hmr_inc,
  cpp_args: hmr_warn_args)

hmr_runtime_dep = declare_dependency(
  include_directories: hmr_inc,
  link_with: libhmr_runtime)

# ---------------------------------------------------------------------------
# hmr_optimizer — rule-level AST optimizer (pure AST transforms, no LLVM).
# Built unconditionally so `hmrc parse/optimize` work in a no-LLVM build.
# ---------------------------------------------------------------------------
libhmr_optimizer = static_library('hmr_optimizer',
  'src/optimizer/optimizer.cpp',
  include_directories: hmr_inc,
  dependencies: hmr_frontend_dep,
  cpp_args: hmr_warn_args)

hmr_optimizer_dep = declare_dependency(
  include_directories: hmr_inc,
  link_with: libhmr_optimizer,
  dependencies: hmr_frontend_dep)

# ---------------------------------------------------------------------------
# hmr_interp — AST tree-walking interpreter that mirrors the IR generator. It
# drives the same hmr_rt_* runtime as a compiled module, so it is both the
# baseline for the "compiled vs interpreter" benchmark and the differential
# oracle for the fuzzer. No LLVM dependency (front-end + runtime only), so it is
# always available, even in a no-LLVM build.
# ---------------------------------------------------------------------------
libhmr_interp = static_library('hmr_interp',
  'src/interp/interpreter.cpp',
  include_directories: hmr_inc,
  dependencies: [hmr_frontend_dep, hmr_runtime_dep],
  cpp_args: hmr_warn_args)

hmr_interp_dep = declare_dependency(
  include_directories: hmr_inc,
  link_with: libhmr_interp,
  dependencies: [hmr_frontend_dep, hmr_runtime_dep])

# ---------------------------------------------------------------------------
# LLVM-backed layers — codegen (AST → IR), backend (IR → object), module
# loader (dlopen + RCU hot-swap) and the pipeline Facade. The front-end CLI
# subcommands work without LLVM; `dump-ir`/`compile` need it, so they are gated
# behind -Denable_llvm (which also defines HMR_HAVE_LLVM for the sources).
# ---------------------------------------------------------------------------
hmrc_deps = [hmr_frontend_dep, hmr_optimizer_dep]
hmrc_args = hmr_warn_args
enable_llvm = get_option('enable_llvm')

if enable_llvm
  # llvm-config resolves transitive component deps, so the high-level modules
  # below pull in everything codegen/backend need (analysis, transforms, the
  # native target's codegen, …). include_type:'system' keeps LLVM's own
  # headers out of our -Wconversion/-Wshadow diagnostics.
  llvm_dep = dependency('llvm',
    version: '>=15.0',
    modules: ['core', 'irreader', 'asmparser', 'passes', 'native',
              'codegen', 'target', 'mc', 'support', 'targetparser'],
    include_type: 'system')

  libhmr_codegen = static_library('hmr_codegen',
    'src/codegen/ir_generator.cpp',
    include_directories: hmr_inc,
    dependencies: [hmr_frontend_dep, hmr_optimizer_dep, llvm_dep],
    cpp_args: hmr_warn_args)
  hmr_codegen_dep = declare_dependency(
    include_directories: hmr_inc,
    link_with: libhmr_codegen,
    dependencies: [hmr_frontend_dep, hmr_optimizer_dep, llvm_dep])

  libhmr_backend = static_library('hmr_backend',
    ['src/backend/backend.cpp', 'src/backend/linker.cpp'],
    include_directories: hmr_inc,
    dependencies: llvm_dep,
    cpp_args: hmr_warn_args)
  hmr_backend_dep = declare_dependency(
    include_directories: hmr_inc,
    link_with: libhmr_backend,
    dependencies: llvm_dep)

  # The module loader uses dlopen/dlsym; no LLVM dependency of its own, but it
  # is only useful alongside compiled modules, so it lives in this block.
  dl_dep = cpp.find_library('dl', required: false)
  libhmr_module = static_library('hmr_module',
    'src/module/module_manager.cpp',
    include_directories: hmr_inc,
    dependencies: dl_dep,
    cpp_args: hmr_warn_args)
  hmr_module_dep = declare_dependency(
    include_directories: hmr_inc,
    link_with: libhmr_module,
    dependencies: dl_dep)

  libhmr_pipeline = static_library('hmr_pipeline',
    'src/pipeline/compiler.cpp',
    include_directories: hmr_inc,
    dependencies: [hmr_frontend_dep, hmr_optimizer_dep, hmr_codegen_dep,
                   hmr_backend_dep, llvm_dep],
    cpp_args: hmr_warn_args)
  hmr_pipeline_dep = declare_dependency(
    include_directories: hmr_inc,
    link_with: libhmr_pipeline,
    dependencies: [hmr_frontend_dep, hmr_optimizer_dep, hmr_codegen_dep,
                   hmr_backend_dep, llvm_dep])

  hmrc_deps += [hmr_codegen_dep, hmr_backend_dep, hmr_pipeline_dep]
  hmrc_args += '-DHMR_HAVE_LLVM=1'
endif

# ---------------------------------------------------------------------------
# hmrc — the compiler driver / CLI.
# ---------------------------------------------------------------------------
hmrc = executable('hmrc', 'src/tools/hmrc.cpp',
  dependencies: hmrc_deps,
  cpp_args: hmrc_args)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if get_option('build_tests')
  subdir('tests')
endif
