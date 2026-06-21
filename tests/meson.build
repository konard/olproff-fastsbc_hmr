# SPDX-License-Identifier: MIT
#
# Front-end test suite. For now: a data-driven `samples` suite that parses every
# bundled .hmr file with the ANTLR-backed parser, asserting the valid corpus
# parses and the invalid_* corpus is rejected (error recovery surfaces the
# diagnostics rather than crashing). The unit suite is wired up in a later
# milestone as the runtime/codegen layers are converted to snake_case.

samples_dir = meson.current_source_dir() / 'hmr_samples'

parse_one = executable('parse_one', 'parse_one.cpp',
  dependencies: hmr_frontend_dep,
  cpp_args: hmr_warn_args)

# Unit suite — self-contained (tests/Test.hpp), one executable per area.
test_matchers = executable('test_matchers', 'test_matchers.cpp',
  dependencies: hmr_runtime_dep,
  cpp_args: hmr_warn_args)
test('matchers', test_matchers, suite: 'unit')

test_runtime = executable('test_runtime', 'test_runtime.cpp',
  dependencies: hmr_runtime_dep,
  cpp_args: hmr_warn_args)
test('runtime', test_runtime, suite: 'unit')

test_value = executable('test_value', 'test_value.cpp',
  dependencies: hmr_frontend_dep,
  cpp_args: hmr_warn_args)
test('value', test_value, suite: 'unit')

test_parser = executable('test_parser', 'test_parser.cpp',
  dependencies: hmr_frontend_dep,
  cpp_args: hmr_warn_args)
test('parser', test_parser, suite: 'unit')

test_optimizer = executable('test_optimizer', 'test_optimizer.cpp',
  dependencies: [hmr_frontend_dep, hmr_optimizer_dep],
  cpp_args: hmr_warn_args)
test('optimizer', test_optimizer, suite: 'unit')

# End-to-end codegen tests — only meaningful with the LLVM backend. The compiled
# .so is dlopen'd at runtime and resolves its hmr_rt_* imports against this test
# binary, so it must export its dynamic symbols (-rdynamic) and link the runtime
# (referencing make_context force-links the TU that defines every hmr_rt_*).
if enable_llvm
  test_codegen = executable('test_codegen', 'test_codegen.cpp',
    dependencies: [hmr_pipeline_dep, hmr_backend_dep, hmr_module_dep,
                   hmr_runtime_dep],
    cpp_args: hmr_warn_args + ['-DHMR_SAMPLES_DIR="' + samples_dir + '"'],
    export_dynamic: true)
  test('codegen', test_codegen, suite: 'unit', timeout: 120)
endif

valid_samples = [
  'all_variables',
  'anonymize_from',
  'full_example',
  'ip_matching',
  'minimal_ruleset',
  'multiple_headers',
  'nested_elements',
  'normalize_pai',
  'regex_captures',
  'topology_hiding',
]

invalid_samples = [
  'invalid_missing_root',
  'invalid_unknown_action',
  'invalid_unterminated_string',
]

foreach s : valid_samples
  test('parse-' + s, parse_one,
    args: [samples_dir / (s + '.hmr')],
    suite: 'samples')
endforeach

foreach s : invalid_samples
  test('reject-' + s, parse_one,
    args: ['--expect-fail', samples_dir / (s + '.hmr')],
    suite: 'samples')
endforeach
