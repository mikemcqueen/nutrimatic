project(
  'nutrimatic', ['cpp'],
  version: '0.1',
  default_options: [
    'cpp_std=c++17',
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

dfs_class_list_lib = library(
  'dfs-class-list',
  ['dfs-class-list.cpp', 'dfs-search.cpp', 'dfs-output.cpp'],
  link_with: index_lib,
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
  link_with: [optparse_lib, dfs_class_list_lib, index_lib], install: true)

# Measurement only, not part of the tool.  See findings/phrase-recovery-cost.md.
executable('measure-f', 'measure-f.cpp',
  link_with: [optparse_lib, index_lib], install: false)

test_dfs_class_list = executable(
  'test-dfs-class-list', 'test-dfs-class-list.cpp',
  link_with: [dfs_class_list_lib, index_lib],
  install: false,
)
test('dfs-class-list', test_dfs_class_list)

test_dfs_search = executable(
  'test-dfs-search', 'test-dfs-search.cpp',
  link_with: [dfs_class_list_lib, index_lib],
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
  install: false,
)
test('dfs-output', test_dfs_output)

make_dfs_test_index = executable(
  'make-dfs-test-index', 'make-dfs-test-index.cpp',
  link_with: index_lib,
  install: false,
)
bash = find_program('bash')
test(
  'dfs-cli',
  bash,
  args: [
    files('test-dfs-cli.sh'),
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

foreach p : ['find-phone-words']
  executable(p, p + '.cpp', link_with: [search_lib, index_lib], install: true)
endforeach

foreach p : ['find-expr', 'test-expr']
  executable(p, p + '.cpp', link_with: [expr_lib, search_lib, index_lib], dependencies: fst_dep, install: true)
endforeach
