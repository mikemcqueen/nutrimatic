project(
  'nutrimatic', ['cpp'],
  version: '0.1',
  default_options: [
    # Documentation only.  MesonToolchain writes cpp_std into
    # build/dep-info/conan_meson_native.ini from conanfile.py's configure(),
    # and a native file's built-in options override these defaults.
    'cpp_std=c++20',
    'warning_level=3',
    'werror=true',
    # The search spends nearly all its time crossing between these libraries:
    # SearchDriver::step() calls IndexReader::children() once per step and the
    # filter's has_transition() once per surviving child.  Through a shared
    # library neither can be inlined and every call goes through the PLT.
    # Linking statically with LTO lets them inline into step(); worth ~7% on
    # find-anagrams.  See findings/perf-analysis.md.
    'default_library=static',
    'b_lto=true',
  ],
)

# Disable warnings that openfst headers trigger
if meson.get_compiler('cpp').get_id() == 'clang'
  # clang-only args
  extra_toolchain_args = ['-Wno-deprecated-copy']
else
  # gcc-only args
  extra_toolchain_args = [
    '-Wno-dangling-pointer',
    '-Wno-missing-template-keyword',
  ]
endif
project_args = [
  '-Wno-ignored-qualifiers',
  '-Wno-sign-compare',
  '-Wno-overloaded-virtual',
  '-Wno-unused-parameter',
  '-Wno-vla',
] + extra_toolchain_args

# Plain x86-64 has no POPCNT, so __builtin_popcountll in SearchDriver::collect()
# compiles to a call into libgcc -- 6% of find-anagrams' runtime, spent entirely
# on the call.  The v2 level is what turns that into the instruction; it also
# picks up SSE4.2, and is worth ~12% overall.  Every x86-64 part since Nehalem
# (2009) and Bulldozer (2011) has it, and measuring -march=native on top of it
# found nothing further.  See findings/perf-analysis.md.
if host_machine.cpu_family() == 'x86_64'
  project_args += ['-march=x86-64-v2']
endif

add_project_arguments(
  project_args,
  language: ['cpp'],
)

fst_dep = dependency('openfst')
tre_dep = dependency('tre')
xml2_dep = dependency('libxml-2.0')
thread_dep = dependency('threads')

# Public-domain getopt_long work-alike (https://github.com/skeeto/optparse),
# vendored because getopt.h is POSIX-only.  Built with warnings off since it
# isn't our code to fix.
optparse_lib = static_library(
  'optparse',
  ['optparse.cpp'],
  override_options: ['warning_level=0', 'werror=false'],
)

index_lib = library(
  'index',
  ['index-reader.cpp', 'index-walker.cpp', 'index-writer.cpp'],
)

coherence_score_lib = static_library(
  'coherence-score', 'coherence-score.cpp',
  install: false,
)

coherence_measure_lib = static_library(
  'coherence-measure', 'coherence-measure.cpp',
  install: false,
)

segment_report_lib = static_library(
  'segment-report', 'segment-report.cpp',
  install: false,
)

dfs_class_list_lib = library(
  'dfs-class-list',
  [
    'dfs-class-list.cpp',
    'dfs-class-list-build.cpp',
    'dfs-cli-args.cpp',
    'dfs-cli-help.cpp',
    'dfs-diagnostic.cpp',
    'dfs-score.cpp',
    'tail-map.cpp',
    'dfs-solo-words.cpp',
    'dfs-search.cpp',
    'dfs-all-runner.cpp',
    'dfs-any-runner.cpp',
    'dfs-search-projected.cpp',
    'dfs-output.cpp',
    'classified.cpp',
    'log.cpp',
    'letter-bag.cpp',
    'row-input.cpp',
  ],
  link_with: index_lib,
  dependencies: thread_dep,
)

# Shared source dependency for small pair-selecting CLIs. Each consumer
# compiles the option/loading helper directly; no separately linked library is
# needed for this small interface.
pair_exclusions_dep = declare_dependency(
  sources: files('pair-exclusions.cpp'),
  link_with: dfs_class_list_lib,
  dependencies: thread_dep,
)

segment_output_dep = declare_dependency(
  sources: files('segment-output.cpp'),
)

segment_rows_dep = declare_dependency(
  sources: files('segment-rows.cpp'),
)

segment_counts_dep = declare_dependency(
  sources: files('segment-counts.cpp'),
  dependencies: [pair_exclusions_dep, segment_output_dep, segment_rows_dep],
)

search_lib = library(
  'search',
  ['search-driver.cpp', 'search-printer.cpp'],
  link_with: [index_lib],
)

expr_lib = library(
  'expr', [
    'expr-anagram.cpp',
    'expr-filter.cpp',
    'expr-intersect.cpp',
    'expr-optimize.cpp',
    'expr-parse.cpp'
  ],
  link_with: [search_lib],
  dependencies: fst_dep,
)

foreach p : ['remove-markup']
  executable(p, p + '.cpp', dependencies: [tre_dep, xml2_dep], install: true)
endforeach

foreach p : ['make-index', 'merge-indexes', 'dump-index', 'explore-index']
  executable(p, p + '.cpp', link_with: index_lib, install: true)
endforeach

find_anagrams = executable('find-anagrams', 'find-anagrams.cpp',
  link_with: [optparse_lib, search_lib, index_lib], install: true)

dfs_anagrams = executable('dfs-anagrams', 'dfs-anagrams.cpp',
  link_with: [optparse_lib, dfs_class_list_lib, index_lib,
              segment_report_lib],
  dependencies: thread_dep,
  install: true)

rerank_anagrams = executable('rerank-anagrams', 'rerank-anagrams.cpp',
  link_with: [optparse_lib, dfs_class_list_lib, index_lib],
  dependencies: [thread_dep, pair_exclusions_dep, segment_rows_dep],
  install: true)

weighted_segments = executable('weighted-segments', 'weighted-segments.cpp',
  link_with: segment_report_lib,
  install: true)

top_segments = executable('top-segments', 'top-segments.cpp',
  dependencies: [pair_exclusions_dep, segment_counts_dep, segment_output_dep],
  install: true)

common_segments = executable('common-segments', 'common-segments.cpp',
  dependencies: [pair_exclusions_dep, segment_output_dep, segment_rows_dep],
  install: true)

find_dupes = executable('find-dupes', 'find-dupes.cpp',
  link_with: dfs_class_list_lib,
  dependencies: [segment_output_dep, thread_dep],
  install: true)

pcomm = executable('pcomm', 'pcomm.cpp', install: true)

executable('pfilter', 'pfilter.cpp',
  link_with: [dfs_class_list_lib, optparse_lib],
  dependencies: thread_dep,
  install: true)

executable('bag-filter', 'bag-filter.cpp',
  link_with: [dfs_class_list_lib, optparse_lib],
  dependencies: thread_dep,
  install: true)

bag_subset = executable('bag-subset', 'bag-subset.cpp',
  link_with: [dfs_class_list_lib, optparse_lib],
  dependencies: thread_dep,
  install: true)

combine_pairs = executable('combine-pairs', 'combine-pairs.cpp',
  link_with: [dfs_class_list_lib, optparse_lib],
  dependencies: thread_dep,
  install: true)

executable('freqsort', 'freqsort.cpp',
  link_with: [dfs_class_list_lib, optparse_lib],
  dependencies: thread_dep,
  install: true)

pairs = executable('pairs', 'pairs.cpp',
  link_with: [dfs_class_list_lib, optparse_lib],
  dependencies: thread_dep,
  install: true)

segment_stats = executable('segment-stats', 'segment-stats.cpp',
  link_with: [dfs_class_list_lib, optparse_lib, index_lib],
  dependencies: [segment_output_dep, segment_rows_dep, thread_dep],
  install: true)

first_segments = executable('first-segments', 'first-segments.cpp',
  dependencies: [pair_exclusions_dep, segment_output_dep, segment_rows_dep],
  install: true)

filter_segments = executable('filter-segments', 'filter-segments.cpp',
  dependencies: [pair_exclusions_dep, segment_output_dep, segment_rows_dep],
  install: true)

# Keep the class-visitor logic in the hot caller rather than leaving it as a
# separate call in the final binary.  This runs as part of the normal build so
# a regression is caught even when the test suite is not run.
bash = find_program('bash')
nm = find_program('nm', native: true)
custom_target(
  'dfs-anagrams-no-visit-fitting-class',
  input: dfs_anagrams,
  output: 'dfs-anagrams-no-visit-fitting-class.stamp',
  command: [bash, files('check-no-symbol.sh'), nm, '@INPUT@', '@OUTPUT@'],
  build_by_default: true,
)

query_index = executable('query-index', 'query-index.cpp',
  link_with: [optparse_lib, dfs_class_list_lib, index_lib],
  dependencies: thread_dep,
  install: true)

# Measurement only, not part of the tool.  See findings/phrase-recovery-cost.md.
executable('measure-f', 'measure-f.cpp',
  link_with: [optparse_lib, index_lib], install: false)

measure_coherence = executable(
  'measure-coherence', 'measure-coherence.cpp',
  link_with: [
    optparse_lib,
    index_lib,
    coherence_score_lib,
    coherence_measure_lib,
  ],
  install: false,
)

test_index_counts = executable(
  'test-index-counts', 'test-index-counts.cpp',
  link_with: index_lib,
  install: false,
)
test('index-counts', test_index_counts)

test_coherence_score = executable(
  'test-coherence-score', 'test-coherence-score.cpp',
  link_with: coherence_score_lib,
  install: false,
)
test('coherence-score', test_coherence_score)

test_measure_coherence = executable(
  'test-measure-coherence', 'test-measure-coherence.cpp',
  link_with: [
    optparse_lib,
    index_lib,
    coherence_score_lib,
    coherence_measure_lib,
  ],
  install: false,
)
test('measure-coherence', test_measure_coherence)

test_dfs_class_list = executable(
  'test-dfs-class-list', 'test-dfs-class-list.cpp',
  link_with: [dfs_class_list_lib, index_lib],
  dependencies: thread_dep,
  install: false,
)
test('dfs-class-list', test_dfs_class_list)

test_dfs_search = executable(
  'test-dfs-search', 'test-dfs-search.cpp',
  link_with: [dfs_class_list_lib, index_lib],
  dependencies: thread_dep,
  install: false,
)
test('dfs-search', test_dfs_search)
test(
  'dfs-search-14',
  test_dfs_search,
  args: ['--validate-14'],
  timeout: 120,
)

test_dfs_output = executable(
  'test-dfs-output', 'test-dfs-output.cpp',
  link_with: [dfs_class_list_lib, index_lib],
  dependencies: thread_dep,
  install: false,
)
test('dfs-output', test_dfs_output)

test_dfs_solo_words = executable(
  'test-dfs-solo-words', 'test-dfs-solo-words.cpp',
  link_with: [dfs_class_list_lib, index_lib],
  dependencies: thread_dep,
  install: false,
)
test('dfs-solo-words', test_dfs_solo_words)

make_dfs_test_index = executable(
  'make-dfs-test-index', 'make-dfs-test-index.cpp',
  link_with: index_lib,
  install: false,
)
test(
  'dfs-cli',
  bash,
  args: [
    files('test-dfs-cli.sh'),
    dfs_anagrams,
    make_dfs_test_index,
    query_index,
  ],
)
test(
  'rerank-anagrams',
  bash,
  args: [
    files('test-rerank-anagrams.sh'),
    rerank_anagrams,
    dfs_anagrams,
    make_dfs_test_index,
  ],
)
test(
  'dfs-cli-differential',
  bash,
  args: [
    files('test-dfs-cli-differential.sh'),
    dfs_anagrams,
    find_anagrams,
  ],
  timeout: 30,
)
test(
  'weighted-segments',
  bash,
  args: [files('test-weighted-segments.sh'), weighted_segments],
)
test(
  'top-segments',
  bash,
  args: [files('test-top-segments.sh'), top_segments],
)
test(
  'common-segments',
  bash,
  args: [files('test-common-segments.sh'), common_segments],
)
test(
  'find-dupes',
  bash,
  args: [files('test-find-dupes.sh'), find_dupes],
)
test(
  'pairs',
  bash,
  args: [files('test-pairs.sh'), pairs],
)
test(
  'bag-subset',
  bash,
  args: [files('test-bag-subset.sh'), bag_subset],
)
test(
  'combine-pairs',
  bash,
  args: [files('test-combine-pairs.sh'), combine_pairs],
)
test(
  'pcomm',
  bash,
  args: [files('test-pcomm.sh'), pcomm],
)
test(
  'first-segments',
  bash,
  args: [files('test-first-segments.sh'), first_segments],
)
test(
  'filter-segments',
  bash,
  args: [files('test-filter-segments.sh'), filter_segments],
)
test(
  'query-index-cli',
  bash,
  args: [files('test-query-index.sh'), query_index, make_dfs_test_index],
  timeout: 30,
)

foreach p : ['find-phone-words']
  executable(p, p + '.cpp', link_with: [search_lib, index_lib], install: true)
endforeach

executable('find-expr', 'find-expr.cpp',
  link_with: [expr_lib, search_lib, index_lib],
  dependencies: fst_dep, install: true)

test_expr = executable('test-expr', 'test-expr.cpp',
  link_with: [expr_lib, search_lib, index_lib],
  dependencies: fst_dep, install: false)
test('expr', test_expr)
