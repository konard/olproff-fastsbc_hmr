# SPDX-License-Identifier: MIT
#
# Front-end test suite. For now: a data-driven `samples` suite that parses every
# bundled .hmr file with the ANTLR-backed parser, asserting the valid corpus
# parses and the invalid_* corpus is rejected (error recovery surfaces the
# diagnostics rather than crashing). The unit suite is wired up in a later
# milestone as the runtime/codegen layers are converted to snake_case.

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

samples_dir = meson.current_source_dir() / 'hmr_samples'

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
